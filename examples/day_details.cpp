#include <iiCalendar/iiCalendar.h>
#include <iostream>

int main() {
    using namespace iiCalendar;
    try {
        Store store(":memory:");
        Event review;
        review.id = "society-review";
        review.title = "Product review";
        review.schedule =
            TimedSchedule{{Instant::parse("2026-09-19T09:30:00.123456789012345678+09:00"),
                           Instant::parse("2026-09-19T10:15:00.123456789012345678+09:00")},
                          "Asia/Seoul"};
        review.attachments = {{"launch-brief", "Launch brief.pdf", "file:///launch-brief.pdf",
                               "application/pdf", 248000}};
        review.tags = {"Society", "launch"};
        store.put(review, 0);
        Event task = review;
        task.id = "launch-checklist";
        task.title = "Finalize launch checklist";
        task.kind = EventKind::Task;
        task.priority = 1;
        task.percentComplete = 50;
        task.schedule = {};
        task.due = Instant::parse("2026-09-19T17:00:00+09:00");
        store.put(task, 0);
        auto day = dayDetails(store, Date(2026, 9, 19), TimeZone("Asia/Seoul"), 2);
        std::cout << day.date.toString() << " | " << day.zone << "\n"
                  << day.events.total << " events, " << day.tasks.total << " tasks, "
                  << day.fileCount << " files\n";
        for (const auto &item : day.events.preview)
            std::cout << item.event.title << ": " << item.occurrence.range.start.toString() << '\n';
        CalendarRegistry calendars;
        std::cout << "Calendar systems:";
        for (const auto &id : calendars.ids())
            std::cout << ' ' << id;
        std::cout << "\nTime-zone database: " << TimeZone::databaseVersion() << '\n';
        auto clock = Clock::sample();
        std::cout << "OS clock: " << clock.wallTime.toString() << " via " << clock.source << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
