#include "Checked.h"
#include "Icu.h"
#include <algorithm>
#include <iiCalendar/Calendar.h>
#include <unicode/uenum.h>

namespace iiCalendar {
namespace {
class NativeCalendar final : public CalendarSystem {
    std::string id_;
    bool julian() const { return id_ == "julian"; }
    unsigned maxDay(int y, unsigned m) const {
        if (julian() && m == 2)
            return y % 4 == 0 ? 29 : 28;
        return Date::daysInMonth(y, m);
    }

  public:
    explicit NativeCalendar(std::string id) : id_(std::move(id)) {}
    std::string id() const override { return id_; }
    std::int64_t toEpochDay(CalendarDate d) const override {
        if (d.year < -999999 || d.year > 999999 || d.month < 1 || d.month > 12 || d.day < 1 ||
            d.leapMonth || d.day > maxDay(d.year, d.month))
            throw std::invalid_argument("invalid native calendar date");
        if (!julian())
            return Date(d.year, d.month, d.day).epochDay();
        const auto a = (14 - static_cast<std::int64_t>(d.month)) / 12,
                   y = static_cast<std::int64_t>(d.year) + 4800 - a,
                   m = static_cast<std::int64_t>(d.month) + 12 * a - 3;
        return d.day + (153 * m + 2) / 5 + 365 * y + detail::floorDiv(y, 4) - 32083 - 2440588;
    }
    CalendarDate fromEpochDay(std::int64_t day) const override {
        if (!julian()) {
            auto d = Date::fromEpochDay(day);
            return {d.year(),
                    d.month(),
                    d.day(),
                    false,
                    d.year() > 0 ? 1 : 0,
                    d.year() > 0 ? d.year() : 1 - d.year()};
        }
        if (day < -366'000'000 || day > 366'000'000)
            throw std::out_of_range("Julian date range exceeded");
        const auto c = day + 2440588 + 32082, d = detail::floorDiv(4 * c + 3, 1461),
                   e = c - detail::floorDiv(1461 * d, 4), m = (5 * e + 2) / 153;
        const auto y = d - 4800 + m / 10, month = m + 3 - 12 * (m / 10),
                   date = e - (153 * m + 2) / 5 + 1;
        CalendarDate result{static_cast<int>(y),
                            static_cast<unsigned>(month),
                            static_cast<unsigned>(date),
                            false,
                            y > 0 ? 1 : 0,
                            static_cast<int>(y > 0 ? y : 1 - y)};
        (void)toEpochDay(result);
        return result;
    }
    CalendarDate addMonths(CalendarDate date, int months, MonthOverflow policy) const override {
        (void)toEpochDay(date);
        const auto n = static_cast<std::int64_t>(date.year) * 12 + date.month - 1 + months;
        const auto y = detail::floorDiv(n, 12);
        if (y < -999999 || y > 999999)
            throw std::out_of_range("year range exceeded");
        date.year = static_cast<int>(y);
        date.month = static_cast<unsigned>(detail::mod(n, 12) + 1);
        const auto max = maxDay(date.year, date.month);
        if (date.day > max && policy == MonthOverflow::Reject)
            throw std::invalid_argument("day absent in target month");
        date.day = std::min(date.day, max);
        return fromEpochDay(toEpochDay(date));
    }
};
CalendarDate fields(const icu::Calendar &cal) {
    UErrorCode s = U_ZERO_ERROR;
    CalendarDate result{cal.get(UCAL_EXTENDED_YEAR, s),
                        static_cast<unsigned>(cal.get(UCAL_MONTH, s) + 1),
                        static_cast<unsigned>(cal.get(UCAL_DATE, s)),
                        cal.get(UCAL_IS_LEAP_MONTH, s) != 0,
                        cal.get(UCAL_ERA, s),
                        cal.get(UCAL_YEAR, s)};
    detail::icuCheck(s);
    return result;
}
class IcuCalendar final : public CalendarSystem {
    std::string id_;
    std::unique_ptr<icu::Calendar> atDate(CalendarDate d) const {
        if (d.month < 1 || d.month > 13 || d.day < 1 || d.day > 31)
            throw std::invalid_argument("invalid calendar fields");
        auto cal = detail::calendar(id_);
        cal->clear();
        cal->set(UCAL_EXTENDED_YEAR, d.year);
        cal->set(UCAL_MONTH, static_cast<int>(d.month) - 1);
        cal->set(UCAL_DATE, static_cast<int>(d.day));
        cal->set(UCAL_IS_LEAP_MONTH, d.leapMonth ? 1 : 0);
        UErrorCode s = U_ZERO_ERROR;
        (void)cal->getTime(s);
        detail::icuCheck(s);
        auto actual = fields(*cal);
        if (actual.year != d.year || actual.month != d.month || actual.day != d.day ||
            actual.leapMonth != d.leapMonth)
            throw std::invalid_argument("calendar date does not exist");
        return cal;
    }

  public:
    explicit IcuCalendar(std::string id) : id_(std::move(id)) {}
    std::string id() const override { return id_; }
    CalendarDate fromEpochDay(std::int64_t day) const override {
        if (day < -100'000'000 || day > 100'000'000)
            throw std::out_of_range("ICU day range exceeded");
        auto cal = detail::calendar(id_);
        UErrorCode s = U_ZERO_ERROR;
        cal->setTime(static_cast<double>(day) * 86400000.0, s);
        detail::icuCheck(s);
        return fields(*cal);
    }
    std::int64_t toEpochDay(CalendarDate date) const override {
        auto cal = atDate(date);
        UErrorCode s = U_ZERO_ERROR;
        const auto ms = cal->getTime(s);
        detail::icuCheck(s);
        if (ms < -8.64e15 || ms > 8.64e15)
            throw std::out_of_range("ICU day range exceeded");
        return static_cast<std::int64_t>(ms / 86400000.0);
    }
    CalendarDate addMonths(CalendarDate date, int months, MonthOverflow policy) const override {
        auto cal = atDate(date);
        UErrorCode s = U_ZERO_ERROR;
        cal->add(UCAL_MONTH, months, s);
        detail::icuCheck(s);
        auto result = fields(*cal);
        if (policy == MonthOverflow::Reject && result.day != date.day)
            throw std::invalid_argument("day absent in target month");
        return result;
    }
};
} // namespace
CalendarRegistry::CalendarRegistry() {
    for (const char *id : {"gregorian", "iso8601", "julian"})
        add(std::make_shared<NativeCalendar>(id));
    UErrorCode s = U_ZERO_ERROR;
    std::unique_ptr<UEnumeration, decltype(&uenum_close)> ids(
        ucal_getKeywordValuesForLocale("calendar", "und", false, &s), uenum_close);
    detail::icuCheck(s);
    int32_t len = 0;
    while (const char *id = uenum_next(ids.get(), &len, &s)) {
        std::string value(id, static_cast<std::size_t>(len));
        if (!systems_.contains(value))
            add(std::make_shared<IcuCalendar>(value));
    }
    detail::icuCheck(s);
}
void CalendarRegistry::add(std::shared_ptr<const CalendarSystem> system) {
    if (!system || system->id().empty())
        throw std::invalid_argument("calendar must have an id");
    if (!systems_.emplace(system->id(), system).second)
        throw std::invalid_argument("calendar id already registered");
}
const CalendarSystem &CalendarRegistry::get(const std::string &id) const {
    auto it = systems_.find(id);
    if (it == systems_.end())
        throw std::invalid_argument("unsupported calendar: " + id);
    return *it->second;
}
std::vector<std::string> CalendarRegistry::ids() const {
    std::vector<std::string> result;
    for (const auto &[id, calendar] : systems_) {
        (void)calendar;
        result.push_back(id);
    }
    return result;
}
} // namespace iiCalendar
