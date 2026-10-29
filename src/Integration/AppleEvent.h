#pragma once
#include <iiCalendar/ExternalCalendar.h>
#import <EventKit/EventKit.h>
namespace iiCalendar::detail {
// Private conversion seam; native fixtures use NSDate/NSTimeZone without reading an OS account.
Event appleCalendarEvent(EKEvent *item, const RemoteCalendar &calendar);
}
