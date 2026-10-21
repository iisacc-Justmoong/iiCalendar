#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <unicode/basictz.h>
#include <unicode/calendar.h>
#include <unicode/gregocal.h>
#include <unicode/strenum.h>
#include <unicode/ucal.h>

namespace iiCalendar::detail {
inline void icuCheck(UErrorCode status) {
    if (U_FAILURE(status))
        throw std::invalid_argument(std::string("ICU: ") + u_errorName(status));
}
inline std::string utf8(const icu::UnicodeString &value) {
    std::string result;
    value.toUTF8String(result);
    return result;
}
inline double millis(std::int64_t seconds) {
    // UDate is integral milliseconds within +/- 100,000,000 days. Subseconds stay outside ICU.
    if (seconds < -8'640'000'000'000LL || seconds > 8'640'000'000'000LL)
        throw std::out_of_range("ICU calendar/time-zone conversion range exceeded");
    return static_cast<double>(seconds) * 1000.0;
}
inline std::unique_ptr<icu::Calendar> calendar(const std::string &id) {
    UErrorCode status = U_ZERO_ERROR;
    const auto locale = icu::Locale::createFromName(("en_US_POSIX@calendar=" + id).c_str());
    std::unique_ptr<icu::Calendar> cal(
        icu::Calendar::createInstance(*icu::TimeZone::getGMT(), locale, status));
    icuCheck(status);
    if (!cal)
        throw std::runtime_error("ICU calendar unavailable");
    cal->setLenient(false);
    return cal;
}
} // namespace iiCalendar::detail
