#include <algorithm>
#include <iiCalendar/Event.h>
#include <set>

namespace iiCalendar {
std::vector<Occurrence> expand(const Event &event, TimeRange window,
                               const CalendarRegistry &calendars, ExpansionLimits limits) {
    event.validate(calendars);
    window.validate();
    if (!limits.maximumOccurrences || !limits.maximumScannedDays)
        throw std::invalid_argument("expansion limits must be positive");
    auto base = event.range();
    if (!base || window.start == window.end || event.status == EventStatus::Cancelled)
        return {};
    TimeZone zone(event.zone());
    auto start = zone.at(base->start).local;
    const auto &calendar = calendars.get(event.calendarSystem);
    const auto anchor = calendar.fromEpochDay(start.date.epochDay());
    const auto &rule = event.recurrence;
    const bool allDay = std::holds_alternative<AllDaySchedule>(event.schedule);
    auto length = base->end - base->start;
    std::int64_t dateLength = 0;
    if (allDay) {
        const auto &d = std::get<AllDaySchedule>(event.schedule);
        dateLength = d.end.epochDay() - d.start.epochDay();
    }
    std::vector<Occurrence> result;
    std::set<Instant> seen;
    std::set<Instant> excluded(rule.excluded.begin(), rule.excluded.end());
    auto append = [&](Instant instant) {
        if (excluded.contains(instant) || !seen.insert(instant).second)
            return;
        auto date = zone.at(instant).local.date;
        TimeRange range{instant, instant + length};
        if (allDay)
            range.end = zone.dayRange(date.addDays(dateLength - 1)).end;
        if (range.overlaps(window)) {
            if (result.size() >= limits.maximumOccurrences)
                throw std::length_error("occurrence limit exceeded");
            result.push_back({event.id, range, date, allDay});
        }
    };
    if (rule.frequency == Frequency::None)
        append(base->start);
    else {
        std::size_t scanned = 0;
        unsigned emitted = 0;
        std::int64_t monthIndex = 0;
        auto previous = anchor;
        std::optional<CalendarDate> measuredMonth;
        std::int64_t daysInMonth = 0;
        auto finalDate = zone.at(window.end).local.date.addDays(1);
        const auto weekAnchor =
            start.date.epochDay() -
            static_cast<std::int64_t>((start.date.weekday() + 7 - rule.weekStart) % 7);
        for (auto day = start.date; day <= finalDate; day = day.addDays(1)) {
            if (rule.count && emitted >= *rule.count)
                break;
            if (++scanned > limits.maximumScannedDays)
                throw std::length_error("recurrence scan limit exceeded");
            auto cd = calendar.fromEpochDay(day.epochDay());
            if (cd.year != previous.year || cd.month != previous.month ||
                cd.leapMonth != previous.leapMonth)
                ++monthIndex;
            previous = cd;
            bool match = false;
            switch (rule.frequency) {
            case Frequency::Daily:
                match = (day.epochDay() - start.date.epochDay()) % rule.interval == 0;
                break;
            case Frequency::Weekly:
                match = ((day.epochDay() - weekAnchor) / 7) % rule.interval == 0;
                break;
            case Frequency::Monthly:
                match = monthIndex % rule.interval == 0;
                break;
            case Frequency::Yearly:
                match = (static_cast<std::int64_t>(cd.year) - anchor.year) % rule.interval == 0;
                break;
            case Frequency::None:
                break;
            }
            if (!match)
                continue;
            if (!rule.months.empty() &&
                std::find(rule.months.begin(), rule.months.end(), cd.month) == rule.months.end())
                continue;
            if (rule.frequency == Frequency::Yearly && rule.months.empty() &&
                (cd.month != anchor.month || cd.leapMonth != anchor.leapMonth))
                continue;
            if (rule.frequency == Frequency::Weekly && rule.weekdays.empty() &&
                day.weekday() != start.date.weekday())
                continue;
            if ((rule.frequency == Frequency::Monthly || rule.frequency == Frequency::Yearly) &&
                rule.monthDays.empty() && rule.weekdays.empty() && cd.day != anchor.day)
                continue;
            if (!rule.monthDays.empty() || !rule.weekdays.empty()) {
                if (!measuredMonth || measuredMonth->year != cd.year ||
                    measuredMonth->month != cd.month || measuredMonth->leapMonth != cd.leapMonth) {
                    auto first = cd;
                    first.day = 1;
                    auto next = calendar.addMonths(first, 1, MonthOverflow::Clamp);
                    daysInMonth = calendar.toEpochDay(next) - calendar.toEpochDay(first);
                    measuredMonth = cd;
                }
                const auto days = daysInMonth;
                if (!rule.monthDays.empty() &&
                    std::none_of(rule.monthDays.begin(), rule.monthDays.end(),
                                 [&](int d) { return (d > 0 ? d : days + d + 1) == cd.day; }))
                    continue;
                if (!rule.weekdays.empty() &&
                    std::none_of(rule.weekdays.begin(), rule.weekdays.end(), [&](WeekdayRule w) {
                        if (w.weekday != day.weekday())
                            return false;
                        return w.ordinal == 0 ||
                               (w.ordinal > 0
                                    ? static_cast<std::int64_t>((cd.day - 1) / 7 + 1) == w.ordinal
                                    : -((days - cd.day) / 7 + 1) == w.ordinal);
                    }))
                    continue;
            }
            Instant instant;
            // Invalid wall times are omitted rather than silently moved to another hour.
            try {
                instant = day == start.date
                              ? base->start
                              : zone.resolve({day, start.time}, AmbiguousTime::Earlier,
                                             MissingTime::Reject);
            } catch (const std::invalid_argument &) {
                continue;
            }
            if (rule.until && instant > *rule.until)
                break;
            ++emitted;
            append(instant);
        }
    }
    for (auto instant : rule.additional)
        append(instant);
    std::sort(result.begin(), result.end(),
              [](const auto &a, const auto &b) { return a.range.start < b.range.start; });
    return result;
}
} // namespace iiCalendar
