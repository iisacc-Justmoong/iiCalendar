#include <iiCalendar/View.h>
#include <set>

namespace iiCalendar {
CalendarView makeView(const ViewRequest &request, const CalendarRegistry &calendars) {
    if (request.weekStart < 1 || request.weekStart > 7 || request.spanDays < 1 ||
        request.spanDays > 3660)
        throw std::invalid_argument("invalid view week start or span");
    const auto &calendar = calendars.get(request.calendarSystem);
    auto cd = calendar.fromEpochDay(request.anchor.epochDay());
    Date start = request.anchor, end = request.anchor.addDays(1), gridStart = start, gridEnd = end;
    switch (request.kind) {
    case ViewKind::Day:
        break;
    case ViewKind::Week:
        start = start.addDays(-static_cast<int>((start.weekday() + 7 - request.weekStart) % 7));
        end = start.addDays(7);
        break;
    case ViewKind::Month: {
        cd.day = 1;
        start = Date::fromEpochDay(calendar.toEpochDay(cd));
        auto next = calendar.addMonths(cd, 1, MonthOverflow::Clamp);
        end = Date::fromEpochDay(calendar.toEpochDay(next));
        break;
    }
    case ViewKind::Year: {
        cd.month = 1;
        cd.day = 1;
        cd.leapMonth = false;
        start = Date::fromEpochDay(calendar.toEpochDay(cd));
        if (cd.year == std::numeric_limits<int>::max())
            throw std::out_of_range("year exceeds range");
        ++cd.year;
        end = Date::fromEpochDay(calendar.toEpochDay(cd));
        break;
    }
    case ViewKind::Agenda:
    case ViewKind::Timeline:
    case ViewKind::Custom:
        end = start.addDays(request.spanDays);
        break;
    default:
        throw std::invalid_argument("invalid calendar view kind");
    }
    gridStart = start;
    gridEnd = end;
    if (request.kind == ViewKind::Month) {
        gridStart = start.addDays(-static_cast<int>((start.weekday() + 7 - request.weekStart) % 7));
        gridEnd = gridStart.addDays(42);
    }
    CalendarView result{start, end, {}};
    for (auto date = gridStart; date < gridEnd; date = date.addDays(1)) {
        if (result.days.size() >= 3660)
            throw std::length_error("calendar view too large");
        result.days.push_back({date, calendar.fromEpochDay(date.epochDay()), date.weekday(),
                               date >= start && date < end,
                               request.today && date == *request.today});
    }
    return result;
}
DayDetails dayDetails(const Store &store, Date date, const TimeZone &zone, std::size_t previewLimit,
                      ExpansionLimits limits) {
    DayDetails result;
    result.date = date;
    result.zone = zone.id();
    std::set<std::pair<std::string, std::string>> files;
    for (auto &item : store.scheduled(zone.dayRange(date), {}, limits)) {
        DetailSection *section = nullptr;
        switch (item.event.kind) {
        case EventKind::Event:
            section = &result.events;
            break;
        case EventKind::Task:
            section = &result.tasks;
            break;
        case EventKind::Activity:
            section = &result.activity;
            break;
        case EventKind::Note:
            section = &result.notes;
            break;
        case EventKind::Reminder:
            section = &result.reminders;
            break;
        }
        ++section->total;
        if (section->preview.size() < previewLimit)
            section->preview.push_back(item);
        if (item.event.kind == EventKind::Task && item.event.status == EventStatus::Completed)
            ++result.completedTasks;
        for (const auto &file : item.event.attachments)
            if (files.emplace(item.event.calendarId, file.id).second) {
                ++result.fileCount;
                if (result.files.size() < previewLimit)
                    result.files.push_back(file);
            }
    }
    return result;
}
} // namespace iiCalendar
