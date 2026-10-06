#include <iiCalendar/ExternalCalendar.h>
#include <iostream>

// Manual integration check. Uses an intentionally invalid, non-secret token to
// verify the built-in TLS/HTTP path; never loads or changes a user's calendar.
int main() {
    iiCalendar::GoogleCalendarProvider provider([] { return "iiCalendar-fixture-invalid"; });
    try {
        (void)provider.calendars();
        std::cerr << "invalid fixture token was unexpectedly accepted\n";
        return 1;
    } catch (const iiCalendar::ExternalCalendarError &e) {
        if (e.code() != iiCalendar::ExternalCalendarErrorCode::AuthenticationRequired) {
            std::cerr << "HTTPS smoke failed: " << e.what() << '\n';
            return 1;
        }
        std::cout << "Google Calendar HTTPS: TLS request completed; invalid fixture token rejected as authentication required\n";
    }
}
