#pragma once
#include <iiCalendar/Calendar.h>
#include <iiCalendar/TimeZone.h>
#include <map>
#include <optional>
#include <variant>

namespace iiCalendar {
enum class EventKind { Event, Task, Activity, Note, Reminder };
enum class EventStatus { Tentative, Confirmed, InProgress, Completed, Cancelled };
enum class Visibility { Public, Private, Confidential };
enum class ParticipantRole { Required, Optional, Organizer, Resource };
enum class Participation { NeedsAction, Accepted, Declined, Tentative };
enum class Frequency { None, Daily, Weekly, Monthly, Yearly };
struct Participant {
    std::string id, name, address;
    ParticipantRole role = ParticipantRole::Required;
    Participation response = Participation::NeedsAction;
    auto operator<=>(const Participant &) const = default;
};
struct Attachment {
    std::string id, name, uri, mediaType;
    std::uint64_t byteSize = 0;
    auto operator<=>(const Attachment &) const = default;
};
struct Location {
    std::string name, address, uri;
    std::optional<double> latitude, longitude;
    auto operator<=>(const Location &) const = default;
};
struct Alarm {
    Duration beforeStart;
    std::string action = "display";
    std::string description;
    auto operator<=>(const Alarm &) const = default;
};
struct WeekdayRule {
    unsigned weekday = 1;
    int ordinal = 0; // 0=every; +1=first, -1=last weekday in a month.
    auto operator<=>(const WeekdayRule &) const = default;
};
struct RecurrenceRule {
    Frequency frequency = Frequency::None;
    unsigned interval = 1;
    std::optional<unsigned> count;
    std::optional<Instant> until; // Inclusive; mutually exclusive with count.
    std::vector<WeekdayRule> weekdays;
    std::vector<int> monthDays; // 1..31 and -31..-1.
    std::vector<unsigned> months;
    unsigned weekStart = 1;
    std::vector<Instant> excluded, additional;
    auto operator<=>(const RecurrenceRule &) const = default;
};
struct TimedSchedule {
    TimeRange range;
    std::string zone = "UTC";
    auto operator<=>(const TimedSchedule &) const = default;
};
struct AllDaySchedule {
    Date start, end; // End date exclusive; date-only, never a fixed 24h duration.
    std::string zone = "UTC";
    auto operator<=>(const AllDaySchedule &) const = default;
};
using Schedule = std::variant<std::monostate, TimedSchedule, AllDaySchedule>;
struct Event {
    std::string id;
    std::string calendarId = "default"; // Collection/account calendar, not a calendar system.
    std::string calendarSystem = "gregorian";
    std::string title, description, url, color, ownerId;
    EventKind kind = EventKind::Event;
    EventStatus status = EventStatus::Confirmed;
    Visibility visibility = Visibility::Private;
    unsigned priority = 0; // 0=unspecified, 1=highest..9=lowest.
    unsigned percentComplete = 0;
    bool busy = true;
    Schedule schedule;
    std::optional<Instant> due, completed;
    Instant created, updated;
    Location location;
    std::vector<Participant> participants;
    std::vector<Attachment> attachments;
    std::vector<Alarm> alarms;
    std::vector<std::string> tags;
    std::map<std::string, std::string> properties, links;
    RecurrenceRule recurrence;
    std::uint64_t revision = 0; // Managed by Store; not a wall-clock version.
    void validate(const CalendarRegistry &calendars) const;
    std::optional<TimeRange> range() const;
    std::optional<LocalDateTime> localStart() const;
    std::string zone() const;
    bool operator==(const Event &) const = default;
};
struct Occurrence {
    std::string eventId;
    TimeRange range;
    Date localDate;
    bool allDay = false;
    bool operator==(const Occurrence &) const = default;
};
struct ExpansionLimits {
    std::size_t maximumOccurrences = 10000;
    std::size_t maximumScannedDays = 1000000;
};
// Explicitly bounded, deterministic expansion. Exceeding a limit throws.
std::vector<Occurrence> expand(const Event &event, TimeRange window,
                               const CalendarRegistry &calendars, ExpansionLimits limits = {});
} // namespace iiCalendar
