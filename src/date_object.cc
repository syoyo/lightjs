#include "date_object_internal.h"

namespace lightjs {

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

    date->properties["__date_ms__"] = Value(storedMs);
    date->properties["__is_date__"] = Value(true);
    return Value(GCPtr<Object>(static_cast<Object*>(date.get())));
}

Value Date_now(const std::vector<Value>& args) {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return Value(static_cast<double>(milliseconds));
}

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

}  // namespace lightjs
