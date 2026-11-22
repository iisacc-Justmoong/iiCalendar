#include "Http.h"
namespace iiCalendarIntegration::detail {
CalendarHttpGet defaultCalendarHttpGet() {
    return [](const CalendarHttpRequest &) -> CalendarHttpResponse {
        throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported,
                                    "provide CalendarHttpGet or build with IICALENDARINTEGRATION_WITH_CURL=ON");
    };
}
}
