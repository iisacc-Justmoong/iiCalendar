#include <iiCalendar/iiCalendar.h>
#include <iostream>
#include <memory>

class CalendarProvider final : public iiCalendar::ExternalCalendarProvider {
  public:
    std::string id() const override { return "consumer"; }
    std::vector<iiCalendar::RemoteCalendar> calendars() override { return {{"remote", "Installed provider"}}; }
    std::vector<iiCalendar::Event> events(const iiCalendar::RemoteCalendar &, iiCalendar::TimeRange range) override {
        iiCalendar::Event event;
        event.id = "remote-event";
        event.schedule = iiCalendar::TimedSchedule{range, "UTC"};
        return {event};
    }
};

int main() {
    iiCalendar::Store store(":memory:");
    iiCalendar::Event event;
    event.id = "consumer";
    event.kind = iiCalendar::EventKind::Task;
    event.due = iiCalendar::Instant::parse("2026-09-19T00:00:00.000000000000000001Z");
    const auto saved = store.put(event, 0);
    if (store.get(event.id) != saved)
        return 1;
    iiCalendar::CalendarObjectManager manager;
    manager.login({"installed-account", "consumer"}, std::make_shared<CalendarProvider>());
    const auto id = manager.calendars()[0].id;
    manager.loadEvents("installed-account", id, {*saved.due, *saved.due + iiCalendar::Duration(3600)});
    if (manager.calendar(id)->events.size() != 1 || !manager.logout("installed-account")) return 2;
    // Resolve the built-in Google provider through the installed static target.
    iiCalendar::GoogleCalendarProvider google([] { return "fixture-token"; },
        [](const iiCalendar::CalendarHttpRequest &) { return iiCalendar::CalendarHttpResponse{200, R"({"items":[]})"}; });
    if (!google.calendars().empty()) return 3;
    // Pull the native/fallback Apple object into the installed static link too.
    std::cout << "Apple Calendar backend available: " << iiCalendar::AppleCalendarProvider::available() << '\n';
    std::cout << "iiCalendar installed consumer: " << saved.due->toString() << '\n';
    return 0;
}
