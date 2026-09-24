#pragma once
#include <compare>
#include <cstdint>
#include <string>

namespace iiCalendar {
enum class MonthOverflow { Reject, Clamp };
struct WeekDate {
    int year;
    unsigned week;
    unsigned weekday; // ISO Monday=1, Sunday=7
    auto operator<=>(const WeekDate &) const = default;
};

// Proleptic Gregorian, astronomical year numbering (year zero exists).
class Date {
  public:
    Date(int year = 1970, unsigned month = 1, unsigned day = 1);
    static Date fromEpochDay(std::int64_t day);
    static Date fromWeekDate(WeekDate date);
    static Date parse(const std::string &iso);
    int year() const noexcept { return year_; }
    unsigned month() const noexcept { return month_; }
    unsigned day() const noexcept { return day_; }
    std::int64_t epochDay() const noexcept;
    unsigned weekday() const noexcept;
    unsigned dayOfYear() const noexcept;
    WeekDate weekDate() const;
    Date addDays(std::int64_t days) const;
    Date addMonths(int months, MonthOverflow policy = MonthOverflow::Clamp) const;
    std::string toString() const;
    static bool isLeapYear(int year) noexcept;
    static unsigned daysInMonth(int year, unsigned month);
    auto operator<=>(const Date &) const = default;

  private:
    int year_;
    unsigned month_, day_;
};
} // namespace iiCalendar
