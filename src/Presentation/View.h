#pragma once
#include <iiCalendar/Store.h>

namespace iiCalendar {
enum class ViewKind { Day, Week, Month, Year, Agenda, Timeline, Custom };
struct ViewRequest {
    ViewKind kind = ViewKind::Month;
    Date anchor;
    std::string calendarSystem = "gregorian";
    unsigned weekStart = 1;
    std::optional<Date> today;
    unsigned spanDays = 7; // Agenda, Timeline, Custom.
};
struct DayCell {
    Date date;
    CalendarDate calendarDate;
    unsigned weekday;
    bool inPeriod, today;
};
struct CalendarView {
    Date start, end;
    std::vector<DayCell> days;
};
CalendarView makeView(const ViewRequest &request, const CalendarRegistry &calendars);
using DetailItem = ScheduledEvent;
struct DetailSection {
    std::size_t total = 0;
    std::vector<DetailItem> preview;
    bool hasMore() const noexcept { return total > preview.size(); }
};
struct DayDetails {
    Date date;
    std::string zone;
    DetailSection events, tasks, activity, notes, reminders;
    std::size_t completedTasks = 0, fileCount = 0;
    std::vector<Attachment> files;
    bool hasMoreFiles() const noexcept { return fileCount > files.size(); }
};
DayDetails dayDetails(const Store &store, Date date, const TimeZone &zone,
                      std::size_t previewLimit = 3, ExpansionLimits limits = {});
} // namespace iiCalendar
