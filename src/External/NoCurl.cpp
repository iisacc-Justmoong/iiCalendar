#include "Http.h"
namespace iiCalendar::detail {
CalendarHttpGet defaultCalendarHttpGet() {
    return [](const CalendarHttpRequest &) -> CalendarHttpResponse {
        throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported,
                                    "provide CalendarHttpGet or build with IICALENDAR_WITH_CURL=ON");
    };
}
}
