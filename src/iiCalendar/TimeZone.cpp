#include "Checked.h"
#include "Icu.h"
#include <algorithm>
#include <iiCalendar/TimeZone.h>

namespace iiCalendar {
namespace {
std::unique_ptr<icu::TimeZone> zone(const std::string &id) {
    return std::unique_ptr<icu::TimeZone>(
        icu::TimeZone::createTimeZone(icu::UnicodeString::fromUTF8(id)));
}
std::int64_t wallSeconds(LocalDateTime local) {
    return local.date.epochDay() * 86400 + local.time.secondOfDay();
}
} // namespace
TimeZone::TimeZone(std::string id) : id_(std::move(id)) {
    if (id_.empty() || id_.find('\0') != std::string::npos)
        throw std::invalid_argument("empty or invalid zone id");
    UErrorCode s = U_ZERO_ERROR;
    icu::UnicodeString canonical;
    icu::TimeZone::getCanonicalID(icu::UnicodeString::fromUTF8(id_), canonical, s);
    detail::icuCheck(s);
    if (canonical == icu::UnicodeString("Etc/Unknown"))
        throw std::invalid_argument("unknown time zone");
}
TimeZone TimeZone::system() {
    UErrorCode s = U_ZERO_ERROR;
    UChar buffer[512];
    auto size = ucal_getHostTimeZone(buffer, 512, &s);
    detail::icuCheck(s);
    return TimeZone(detail::utf8(icu::UnicodeString(buffer, size)));
}
std::vector<std::string> TimeZone::available() {
    std::unique_ptr<icu::StringEnumeration> values(icu::TimeZone::createEnumeration());
    if (!values)
        throw std::runtime_error("time zones unavailable");
    UErrorCode s = U_ZERO_ERROR;
    int32_t len = 0;
    std::vector<std::string> result;
    while (const auto *value = values->next(&len, s))
        result.emplace_back(value, static_cast<std::size_t>(len));
    detail::icuCheck(s);
    return result;
}
std::string TimeZone::databaseVersion() {
    UErrorCode s = U_ZERO_ERROR;
    auto v = ucal_getTZDataVersion(&s);
    detail::icuCheck(s);
    return v;
}
ZonedDateTime TimeZone::at(Instant instant) const {
    auto tz = zone(id_);
    UErrorCode s = U_ZERO_ERROR;
    int32_t raw = 0, dst = 0;
    tz->getOffset(detail::millis(instant.seconds()), false, raw, dst, s);
    detail::icuCheck(s);
    const int offset = (raw + dst) / 1000;
    const auto seconds = detail::add(instant.seconds(), offset);
    return {{Date::fromEpochDay(detail::floorDiv(seconds, 86400)),
             Time::fromSecondOfDay(static_cast<unsigned>(detail::mod(seconds, 86400)),
                                   instant.fraction())},
            offset,
            dst != 0,
            id_};
}
Instant TimeZone::resolve(LocalDateTime local, AmbiguousTime ambiguous, MissingTime missing) const {
    auto tz = zone(id_);
    auto *basic = dynamic_cast<icu::BasicTimeZone *>(tz.get());
    if (!basic)
        throw std::runtime_error("time zone does not support transition resolution");
    auto seconds = wallSeconds(local);
    std::vector<Instant> candidates, interpreted;
    for (auto option : {UCAL_TZ_LOCAL_FORMER, UCAL_TZ_LOCAL_LATTER}) {
        UErrorCode s = U_ZERO_ERROR;
        int32_t raw = 0, dst = 0;
        basic->getOffsetFromLocal(detail::millis(seconds), option, option, raw, dst, s);
        detail::icuCheck(s);
        Instant candidate(detail::sub(seconds, (raw + dst) / 1000), local.time.fraction());
        interpreted.push_back(candidate);
        if (at(candidate).local == local)
            candidates.push_back(candidate);
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (candidates.size() == 1)
        return candidates.front();
    if (candidates.size() == 2) {
        if (ambiguous == AmbiguousTime::Reject)
            throw std::invalid_argument("ambiguous local time");
        return ambiguous == AmbiguousTime::Earlier ? candidates.front() : candidates.back();
    }
    if (missing == MissingTime::Reject)
        throw std::invalid_argument("nonexistent local time");
    return *std::max_element(interpreted.begin(), interpreted.end());
}
TimeRange TimeZone::dayRange(Date date) const {
    return {resolve({date, Time()}, AmbiguousTime::Earlier, MissingTime::ShiftForward),
            resolve({date.addDays(1), Time()}, AmbiguousTime::Earlier, MissingTime::ShiftForward)};
}
} // namespace iiCalendar
