#pragma once
#include <iiCalendarIntegration/ExternalCalendar.h>
#import <EventKit/EventKit.h>
namespace iiCalendarIntegration::detail {
// Private conversion seam; native fixtures use NSDate/NSTimeZone without reading an OS account.
Event appleCalendarEvent(EKEvent *item, const RemoteCalendar &calendar);
}
