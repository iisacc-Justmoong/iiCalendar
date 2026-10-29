#include "FileProviderAdapter.h"
#include <iiFileProviderPresentation.h>
#include <iostream>
#include <stdexcept>

using namespace iiCalendar;
namespace {
class Fixture final : public ExternalCalendarProvider {
public:
    std::string id() const override { return "fixture"; }
    std::vector<RemoteCalendar> calendars() override { return {}; }
    std::vector<Event> events(const RemoteCalendar &, TimeRange) override { return {}; }
};
void check(bool condition) { if (!condition) throw std::runtime_error("file composition assertion failed"); }
IisaccCalendarSession session(const std::string &owner) {
    return {"https://iisacc.com", owner, "", "session", "device", "", "Owner",
            Clock::sample().wallTime + Duration(3600)};
}
}
int main() {
    try {
        iiFileProvider::native::TemporaryDirectory temporary;
        check(temporary.isValid());
        const auto path = temporary.path() / "connections.json";
        CalendarAccountManager owner;
        owner.setIisaccSession(session("owner"));
        const auto account = owner.connect({"subject", "fixture", "Work"}, std::make_shared<Fixture>());
        owner.setEnabled(account.id, false);
        iiCalendarFileProvider::saveConnections(owner, path);
        check(iiFileProvider::native::File::read(path) == owner.connectionDocument());
        CalendarAccountManager restored;
        restored.setIisaccSession(session("owner"));
        iiCalendarFileProvider::loadConnections(restored, path);
        check(restored.accounts().size() == 2 && !restored.accounts()[1].enabled);
        CalendarAccountManager anonymous;
        try { iiCalendarFileProvider::loadConnections(anonymous, path); throw std::runtime_error("anonymous load accepted"); }
        catch (const ExternalCalendarError &e) { check(e.code() == ExternalCalendarErrorCode::AuthenticationRequired); }
        CalendarAccountManager other;
        other.setIisaccSession(session("other"));
        try { iiCalendarFileProvider::loadConnections(other, path); throw std::runtime_error("other owner accepted"); }
        catch (const ExternalCalendarError &e) { check(e.code() == ExternalCalendarErrorCode::PermissionDenied); }
        iiFileProvider::native::Presentation::cancelPending(path);
        check(iiFileProvider::native::File::remove(path));
        std::cout << "calendar file composition passed\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
