#include <iiCalendar/ExternalCalendar.h>
namespace iiCalendar {
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
void AppleCalendarProvider::requestAccess(std::function<void(bool)>) { unsupported(); }
std::vector<AppleCalendarSource> AppleCalendarProvider::sources() { unsupported(); }
std::vector<RemoteCalendar> AppleCalendarProvider::calendars() { unsupported(); }
std::vector<Event> AppleCalendarProvider::events(const RemoteCalendar &, TimeRange) { unsupported(); }
}
