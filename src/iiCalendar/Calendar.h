#pragma once
#include "Date.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace iiCalendar {
struct CalendarDate {
    int year = 1970; // Extended year, independent of era numbering.
    unsigned month = 1;
    unsigned day = 1;
    bool leapMonth = false;
    int era = 1;
    int yearOfEra = 1970; // Display output; conversion uses extended year.
    auto operator<=>(const CalendarDate &) const = default;
};
// Custom/historical/business calendars implement this interface without ICU types.
class CalendarSystem {
  public:
    virtual ~CalendarSystem() = default;
    virtual std::string id() const = 0;
    virtual CalendarDate fromEpochDay(std::int64_t day) const = 0;
    virtual std::int64_t toEpochDay(CalendarDate date) const = 0;
    virtual CalendarDate addMonths(CalendarDate date, int months, MonthOverflow policy) const = 0;
};
class CalendarRegistry {
  public:
    CalendarRegistry();
    void add(std::shared_ptr<const CalendarSystem> calendar);
    const CalendarSystem &get(const std::string &id) const;
    std::vector<std::string> ids() const;

  private:
    std::map<std::string, std::shared_ptr<const CalendarSystem>, std::less<>> systems_;
};
} // namespace iiCalendar
