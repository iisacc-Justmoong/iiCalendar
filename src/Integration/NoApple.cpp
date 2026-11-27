#include <iiCalendarIntegration/ExternalCalendar.h>
namespace iiCalendarIntegration {
namespace {
[[noreturn]] void unsupported() {
    throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "Apple Calendar EventKit requires macOS or iOS");
}
}
struct AppleCalendarProvider::Impl {};
AppleCalendarProvider::AppleCalendarProvider(AppleCalendarOptions) : impl_(std::make_unique<Impl>()) {}
AppleCalendarProvider::~AppleCalendarProvider() = default;
bool AppleCalendarProvider::available() noexcept { return false; }
AppleCalendarAuthorization AppleCalendarProvider::authorization() { unsupported(); }
AppleCalendarAuthorization AppleCalendarProvider::remindersAuthorization() { unsupported(); }
void AppleCalendarProvider::requestAccess(std::function<void(bool)>) { unsupported(); }
void AppleCalendarProvider::requestRemindersAccess(std::function<void(bool)>) { unsupported(); }
std::vector<AppleCalendarSource> AppleCalendarProvider::sources() { unsupported(); }
std::vector<RemoteCalendar> AppleCalendarProvider::calendars() { unsupported(); }
std::vector<Event> AppleCalendarProvider::events(const RemoteCalendar &, TimeRange) { unsupported(); }
Event AppleCalendarProvider::createEvent(const RemoteCalendar &, const Event &) { unsupported(); }
Event AppleCalendarProvider::updateOccurrence(const RemoteCalendar &, const Event &, const EventOccurrenceUpdate &) { unsupported(); }
std::vector<RemoteTaskList> AppleCalendarProvider::taskLists() { unsupported(); }
Event AppleCalendarProvider::createTask(const RemoteTaskList &, const Event &, bool) { unsupported(); }
}
