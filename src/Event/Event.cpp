#include <algorithm>
#include <cmath>
#include <iiCalendar/Event.h>
#include <set>

namespace iiCalendar {
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::invalid_argument(message);
}
void text(const std::string &value) {
    require(value.size() <= 1'048'576 && value.find('\0') == std::string::npos,
            "invalid or oversized text");
}
template <class T> void count(const T &value) {
    require(value.size() <= 10000, "too many metadata values");
}
template <class T> bool validEnum(T value, int max) {
    return static_cast<int>(value) >= 0 && static_cast<int>(value) <= max;
}
} // namespace
void Event::validate(const CalendarRegistry &calendars) const {
    require(!id.empty() && !calendarId.empty(), "event and calendar ids are required");
    for (const auto *value :
         {&id, &calendarId, &calendarSystem, &title, &description, &url, &color, &ownerId})
        text(*value);
    (void)calendars.get(calendarSystem);
    require(validEnum(kind, 4) && validEnum(status, 4) && validEnum(visibility, 2),
            "invalid event enum");
    require(priority <= 9 && percentComplete <= 100, "invalid priority or completion");
    require(updated >= created, "updated predates created");
    if (const auto *timed = std::get_if<TimedSchedule>(&schedule)) {
        timed->range.validate();
        TimeZone tz(timed->zone);
        (void)tz.at(timed->range.start);
        (void)tz.at(timed->range.end);
    } else if (const auto *days = std::get_if<AllDaySchedule>(&schedule)) {
        require(days->end > days->start, "all-day end date must be exclusive and after start");
        TimeZone tz(days->zone);
        (void)tz.dayRange(days->start);
        (void)tz.dayRange(days->end.addDays(-1));
    }
    require(location.latitude.has_value() == location.longitude.has_value(),
            "coordinates require both latitude and longitude");
    if (location.latitude)
        require(std::isfinite(*location.latitude) && std::isfinite(*location.longitude) &&
                    std::abs(*location.latitude) <= 90 && std::abs(*location.longitude) <= 180,
                "invalid coordinates");
    text(location.name);
    text(location.address);
    text(location.uri);
    count(participants);
    count(attachments);
    count(alarms);
    count(tags);
    count(properties);
    count(links);
    std::set<std::string> ids;
    for (const auto &p : participants) {
        text(p.id);
        text(p.name);
        text(p.address);
        require(!p.id.empty() && ids.insert(p.id).second, "participant ids must be unique");
        require(validEnum(p.role, 3) && validEnum(p.response, 3), "invalid participant enum");
    }
    ids.clear();
    for (const auto &a : attachments) {
        text(a.id);
        text(a.name);
        text(a.uri);
        text(a.mediaType);
        require(!a.id.empty() && ids.insert(a.id).second, "attachment ids must be unique");
    }
    for (const auto &a : alarms) {
        require(a.beforeStart >= Duration(), "alarm lead duration must be nonnegative");
        text(a.action);
        text(a.description);
    }
    ids.clear();
    for (const auto &tag : tags) {
        text(tag);
        require(!tag.empty() && ids.insert(tag).second, "tags must be nonempty and unique");
    }
    for (const auto *map : {&properties, &links})
        for (const auto &[key, value] : *map) {
            text(key);
            text(value);
            require(!key.empty(), "metadata key is required");
        }
    const auto &r = recurrence;
    require(validEnum(r.frequency, 4), "invalid recurrence frequency");
    require(r.interval >= 1 && r.interval <= 100000, "invalid recurrence interval");
    require(r.weekStart >= 1 && r.weekStart <= 7, "invalid first weekday");
    require(!r.count || *r.count > 0, "recurrence count must be positive");
    require(!(r.count && r.until), "count and until are mutually exclusive");
    count(r.weekdays);
    count(r.monthDays);
    count(r.months);
    count(r.excluded);
    count(r.additional);
    if (r.frequency != Frequency::None || !r.additional.empty())
        require(!std::holds_alternative<std::monostate>(schedule),
                "recurrence requires a scheduled start");
    if (r.frequency == Frequency::None)
        require(!r.count && !r.until && r.weekdays.empty() && r.monthDays.empty() &&
                    r.months.empty(),
                "frequency is required for recurrence filters");
    for (auto w : r.weekdays) {
        require(w.weekday >= 1 && w.weekday <= 7 && w.ordinal >= -5 && w.ordinal <= 5,
                "invalid weekday rule");
        if (w.ordinal)
            require(r.frequency == Frequency::Monthly || r.frequency == Frequency::Yearly,
                    "ordinal weekdays need monthly or yearly frequency");
    }
    for (auto m : r.months)
        require(m >= 1 && m <= 13, "invalid recurrence month");
    for (auto d : r.monthDays)
        require(d != 0 && d >= -31 && d <= 31, "invalid recurrence month day");
    if (std::holds_alternative<AllDaySchedule>(schedule))
        for (auto value : r.additional)
            require(TimeZone(zone()).at(value).local.time == Time(),
                    "all-day additions require local midnight");
}
std::string Event::zone() const {
    if (const auto *t = std::get_if<TimedSchedule>(&schedule))
        return t->zone;
    if (const auto *d = std::get_if<AllDaySchedule>(&schedule))
        return d->zone;
    return "UTC";
}
std::optional<TimeRange> Event::range() const {
    if (const auto *t = std::get_if<TimedSchedule>(&schedule))
        return t->range;
    if (const auto *d = std::get_if<AllDaySchedule>(&schedule)) {
        TimeZone tz(d->zone);
        return TimeRange{tz.dayRange(d->start).start, tz.dayRange(d->end.addDays(-1)).end};
    }
    if (due)
        return TimeRange{*due, *due};
    return {};
}
std::optional<LocalDateTime> Event::localStart() const {
    if (auto r = range())
        return TimeZone(zone()).at(r->start).local;
    return {};
}
} // namespace iiCalendar
