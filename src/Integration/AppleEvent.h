#pragma once
#include <iiCalendarIntegration/ExternalCalendar.h>
#import <EventKit/EventKit.h>
namespace iiCalendarIntegration::detail {
// Matching names are only candidates: they never prove two sources share an account.
enum class AppleTaskSourceMatch { None, Exact, Candidate };
AppleTaskSourceMatch classifyAppleTaskSource(const std::string &selectedId, const std::string &selectedName, int selectedType,
                                            const std::string &candidateId, const std::string &candidateName, int candidateType);
// Private conversion seam; native fixtures use NSDate/NSTimeZone without reading an OS account.
Event appleCalendarEvent(EKEvent *item, const RemoteCalendar &calendar);
// Populate a new EventKit event without touching the user's event store.
void applyAppleCalendarDraft(EKEvent *item, const Event &draft);
// Set the destination first, then populate the draft, so assigning a calendar
// cannot replace the caller's availability or alarm choices.
void prepareAppleCalendarDraft(EKEvent *item, EKCalendar *calendar, const Event &draft);
// Modify only title and schedule on an existing EventKit occurrence, retaining
// its alarms, invitees and recurrence. Does not save or access an event store.
void applyAppleOccurrenceUpdate(EKEvent *item, const Event &original, const EventOccurrenceUpdate &change);
// Populate reminder components without accessing a personal calendar account.
void applyAppleReminderDraft(EKReminder *item, const Event &draft);
// Set the destination before reminder dates and other draft fields.
void prepareAppleReminderDraft(EKReminder *item, EKCalendar *calendar, const Event &draft);
}
