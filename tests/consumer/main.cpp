#include <iiCalendarIntegration>
#include <sqlite3.h>
#include <iostream>
#include <memory>

class CalendarProvider final : public iiCalendarIntegration::ExternalCalendarProvider {
  public:
    std::string id() const override { return "consumer"; }
    std::vector<iiCalendarIntegration::RemoteCalendar> calendars() override { return {{"remote", "Installed provider"}}; }
    std::vector<iiCalendarIntegration::Event> events(const iiCalendarIntegration::RemoteCalendar &, iiCalendarIntegration::TimeRange range) override {
        iiCalendarIntegration::Event event;
        event.id = "remote-event";
        event.schedule = iiCalendarIntegration::TimedSchedule{range, "UTC"};
        return {event};
    }
};

int main() {
    sqlite3 *raw = nullptr;
    const auto opened = sqlite3_open_v2(":memory:", &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    std::shared_ptr<sqlite3> connection(raw, [](sqlite3 *value) { (void)sqlite3_close_v2(value); });
    if (opened != SQLITE_OK || !connection) return 8;
    iiCalendarIntegration::Store store(connection);
    iiCalendarIntegration::Event event;
    event.id = "consumer";
    event.kind = iiCalendarIntegration::EventKind::Task;
    event.due = iiCalendarIntegration::Instant::parse("2026-09-19T00:00:00.000000000000000001Z");
    const auto saved = store.put(event, 0);
    if (store.get(event.id) != saved)
        return 1;
    iiCalendarIntegration::CalendarObjectManager manager;
    manager.login({"installed-account", "consumer"}, std::make_shared<CalendarProvider>());
    const auto id = manager.calendars()[0].id;
    manager.loadEvents("installed-account", id, {*saved.due, *saved.due + iiCalendarIntegration::Duration(3600)});
    if (manager.calendar(id)->events.size() != 1 || !manager.logout("installed-account")) return 2;
    // Resolve the built-in Google provider through the installed static target.
    iiCalendarIntegration::GoogleCalendarProvider google([] { return "fixture-token"; },
        [](const iiCalendarIntegration::CalendarHttpRequest &) { return iiCalendarIntegration::CalendarHttpResponse{200, R"({"items":[]})"}; });
    if (!google.calendars().empty()) return 3;
    iiCalendarIntegration::CalendarAccountManager accounts;
    if (accounts.authenticated()) return 4;
    accounts.setIisaccSession(iiCalendarIntegration::IisaccCalendarSession{"https://iisacc.com", "installed-owner", "", "session", "device", "", "Owner",
        iiCalendarIntegration::Clock::sample().wallTime + iiCalendarIntegration::Duration(3600)});
    const auto connected = accounts.connect({"installed-external", "consumer", "Work"}, std::make_shared<CalendarProvider>());
    if (connected.ownerId != "installed-owner" || accounts.accounts().size() != 2 || accounts.calendars().size() != 1) return 5;
    iiCalendarIntegration::MicrosoftCalendarProvider microsoft([] { return "fixture-token"; },
        [](const iiCalendarIntegration::CalendarHttpRequest &) { return iiCalendarIntegration::CalendarHttpResponse{200, R"({"value":[]})"}; });
    if (!microsoft.calendars().empty()) return 6;
    accounts.setIisaccSession(std::nullopt);
    if (!accounts.accounts().empty()) return 7;
    // Pull the native/fallback Apple object into the installed static link too.
    std::cout << "Apple Calendar backend available: " << iiCalendarIntegration::AppleCalendarProvider::available() << '\n';
    std::cout << "iiCalendarIntegration installed consumer: " << saved.due->toString() << '\n';
    return 0;
}
