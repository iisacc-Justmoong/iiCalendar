#include "Checked.h"
#include <algorithm>
#include <charconv>
#include <iiCalendar/Date.h>
#include <iomanip>
#include <sstream>

namespace iiCalendar {
namespace {
// Integer 400-year era decomposition; no std::chrono::year 16-bit range limit.
std::int64_t epoch(int year, unsigned month, unsigned day) noexcept {
    std::int64_t y = year;
    y -= month <= 2;
    const auto era = detail::floorDiv(y, 400), yoe = y - era * 400;
    const auto m = static_cast<std::int64_t>(month) + (month > 2 ? -3 : 9);
    const auto doy = (153 * m + 2) / 5 + day - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
} // namespace
Date::Date(int y, unsigned m, unsigned d) : year_(y), month_(m), day_(d) {
    if (y < -999999 || y > 999999 || m < 1 || m > 12 || d < 1 || d > daysInMonth(y, m))
        throw std::invalid_argument("invalid Gregorian date (supported years -999999..999999)");
}
bool Date::isLeapYear(int year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
unsigned Date::daysInMonth(int year, unsigned month) {
    if (month < 1 || month > 12)
        throw std::invalid_argument("invalid month");
    constexpr unsigned sizes[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return sizes[month - 1] + (month == 2 && isLeapYear(year) ? 1U : 0U);
}
std::int64_t Date::epochDay() const noexcept {
    return epoch(year_, month_, day_);
}
Date Date::fromEpochDay(std::int64_t day) {
    if (day < epoch(-999999, 1, 1) || day > epoch(999999, 12, 31))
        throw std::out_of_range("date range exceeded");
    const auto z = day + 719468, era = detail::floorDiv(z, 146097), doe = z - era * 146097;
    const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    auto y = yoe + era * 400;
    const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    const auto d = doy - (153 * mp + 2) / 5 + 1, m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    return Date(static_cast<int>(y), static_cast<unsigned>(m), static_cast<unsigned>(d));
}
unsigned Date::weekday() const noexcept {
    return static_cast<unsigned>(detail::mod(epochDay() + 3, 7) + 1);
}
unsigned Date::dayOfYear() const noexcept {
    return static_cast<unsigned>(epochDay() - epoch(year_, 1, 1) + 1);
}
Date Date::addDays(std::int64_t days) const {
    return fromEpochDay(detail::add(epochDay(), days));
}
Date Date::addMonths(int months, MonthOverflow policy) const {
    const auto index = static_cast<std::int64_t>(year_) * 12 + month_ - 1 + months;
    const auto year = detail::floorDiv(index, 12);
    const auto month = static_cast<unsigned>(detail::mod(index, 12) + 1);
    if (year < -999999 || year > 999999)
        throw std::out_of_range("date range exceeded");
    auto day = day_;
    const auto max = daysInMonth(static_cast<int>(year), month);
    if (day > max && policy == MonthOverflow::Reject)
        throw std::invalid_argument("day absent in target month");
    return Date(static_cast<int>(year), month, std::min(day, max));
}
WeekDate Date::weekDate() const {
    const auto wd = weekday();
    const auto thursday = addDays(4 - static_cast<int>(wd));
    const auto jan4 = Date(thursday.year(), 1, 4);
    const auto monday = jan4.addDays(1 - static_cast<int>(jan4.weekday()));
    return {thursday.year(), static_cast<unsigned>((epochDay() - monday.epochDay()) / 7 + 1), wd};
}
Date Date::fromWeekDate(WeekDate w) {
    if (w.week < 1 || w.week > 53 || w.weekday < 1 || w.weekday > 7)
        throw std::invalid_argument("invalid ISO week date");
    Date jan4(w.year, 1, 4);
    auto result = jan4.addDays(1 - static_cast<int>(jan4.weekday()) +
                               static_cast<int>((w.week - 1) * 7 + w.weekday - 1));
    if (result.weekDate() != w)
        throw std::invalid_argument("ISO week absent in year");
    return result;
}
std::string Date::toString() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setfill('0');
    if (year_ < 0)
        out << '-' << std::setw(6) << -year_;
    else if (year_ > 9999)
        out << '+' << std::setw(6) << year_;
    else
        out << std::setw(4) << year_;
    out << '-' << std::setw(2) << month_ << '-' << std::setw(2) << day_;
    return out.str();
}
Date Date::parse(const std::string &iso) {
    const bool signedYear = !iso.empty() && (iso[0] == '-' || iso[0] == '+');
    const std::size_t n = signedYear ? 7 : 4;
    if (iso.size() != n + 6 || iso[n] != '-' || iso[n + 3] != '-')
        throw std::invalid_argument("invalid ISO date");
    auto number = [&](std::size_t start, std::size_t count) {
        int v = 0;
        for (std::size_t i = start; i < start + count; ++i) {
            if (iso[i] < '0' || iso[i] > '9')
                throw std::invalid_argument("invalid date digit");
            v = v * 10 + iso[i] - '0';
        }
        return v;
    };
    int y = number(signedYear ? 1 : 0, signedYear ? 6 : 4);
    if (iso[0] == '-')
        y = -y;
    return Date(y, static_cast<unsigned>(number(n + 1, 2)),
                static_cast<unsigned>(number(n + 4, 2)));
}
} // namespace iiCalendar
