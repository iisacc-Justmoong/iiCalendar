#include "../src/Integration/AppleEvent.h"
#include <iostream>

// An Objective-C selector fixture exercises the same native conversion as EKEvent,
// without creating an event store, reading personal data, or prompting for access.
@interface CalendarEventFixture : NSObject { EKCalendar *_calendar; }
@property(nonatomic, copy) NSString *calendarItemIdentifier, *eventIdentifier, *title, *notes, *location;
@property(nonatomic, copy) NSURL *URL;
@property(nonatomic, copy) NSDate *startDate, *endDate, *occurrenceDate, *creationDate, *lastModifiedDate;
@property(nonatomic, copy) NSTimeZone *timeZone;
@property(nonatomic, getter=isAllDay) BOOL allDay;
@property(nonatomic) EKEventAvailability availability;
@property(nonatomic) EKEventStatus status;
@property(nonatomic, strong) EKCalendar *calendar;
@property(nonatomic, copy) NSArray<EKAlarm *> *alarms;
@end
@implementation CalendarEventFixture
- (EKCalendar *)calendar { return _calendar; }
- (void)setCalendar:(EKCalendar *)value {
    _calendar = value;
    self.availability = EKEventAvailabilityBusy;
    self.alarms = @[(EKAlarm *)(id)[NSObject new]];
}
@end
@interface CalendarReminderFixture : NSObject { EKCalendar *_calendar; }
@property(nonatomic, copy) NSString *title, *notes;
@property(nonatomic, copy) NSDateComponents *startDateComponents, *dueDateComponents;
@property(nonatomic, strong) EKCalendar *calendar;
@property(nonatomic, copy) NSArray<EKAlarm *> *alarms;
@end
@implementation CalendarReminderFixture
- (EKCalendar *)calendar { return _calendar; }
- (void)setCalendar:(EKCalendar *)value { _calendar = value; self.title = @""; self.dueDateComponents = nil; self.alarms = @[(EKAlarm *)(id)[NSObject new]]; }
@end

using namespace iiCalendarIntegration;
namespace {
int checks = 0;
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) throw std::runtime_error(#__VA_ARGS__); } while (false)
NSDate *at(const std::string &text) {
    const auto value = Instant::parse(text);
    return [NSDate dateWithTimeIntervalSince1970:static_cast<double>(value.seconds()) + static_cast<double>(value.fraction()) / 1e18];
}
Event convert(CalendarEventFixture *fixture, const RemoteCalendar &calendar) {
    return detail::appleCalendarEvent((EKEvent *)(id)fixture, calendar);
}
}
int main() {
    @autoreleasepool {
        try {
            using Match = detail::AppleTaskSourceMatch;
            CHECK(detail::classifyAppleTaskSource("events", "iCloud", 1, "events", "renamed", 2) == Match::Exact);
            CHECK(detail::classifyAppleTaskSource("events", "iCloud", 1, "reminders", "iCloud", 1) == Match::Candidate);
            CHECK(detail::classifyAppleTaskSource("events", "iCloud", 1, "other", "iCloud", 2) == Match::None);
            CHECK(detail::classifyAppleTaskSource("events", "iCloud", 1, "other", "Other", 1) == Match::None);
            CHECK(detail::classifyAppleTaskSource("events", "", 1, "other", "", 1) == Match::None);
            RemoteCalendar calendar;
            calendar.id = "remote-calendar"; calendar.color = "#57965C"; calendar.sourceId = "icloud-source";
            CalendarEventFixture *fixture = [CalendarEventFixture new];
            Event draft; draft.id = "draft"; draft.title = "New event"; draft.description = "Notes";
            draft.location.name = "Studio"; draft.busy = false;
            draft.schedule = TimedSchedule{{Instant::parse("2026-10-02T00:00:00Z"), Instant::parse("2026-10-02T01:00:00Z")}, "Asia/Seoul"};
            auto destination = (EKCalendar *)(id)[NSObject new];
            detail::prepareAppleCalendarDraft((EKEvent *)(id)fixture, destination, draft);
            CHECK([fixture.title isEqualToString:@"New event"] && [fixture.notes isEqualToString:@"Notes"]);
            CHECK([fixture.location isEqualToString:@"Studio"] && fixture.availability == EKEventAvailabilityFree);
            CHECK(fixture.calendar == destination && fixture.alarms.count == 0);
            CHECK([fixture.startDate isEqualToDate:at("2026-10-02T00:00:00Z")]);
            auto taskDraft = draft; taskDraft.kind = EventKind::Task; taskDraft.location.name.clear();
            CalendarReminderFixture *task = [CalendarReminderFixture new];
            detail::prepareAppleReminderDraft((EKReminder *)(id)task, destination, taskDraft);
            CHECK([task.title isEqualToString:@"New event"] && [task.notes isEqualToString:@"Notes"]);
            CHECK(task.calendar == destination && task.alarms.count == 0);
            CHECK(task.startDateComponents.hour == 9 && task.dueDateComponents.hour == 10);
            CHECK([task.dueDateComponents.timeZone.name isEqualToString:@"Asia/Seoul"]);
            fixture.calendarItemIdentifier = @"series"; fixture.eventIdentifier = @"event";
            fixture.title = @"검토"; fixture.notes = @"설명"; fixture.location = @"Seoul";
            fixture.URL = [NSURL URLWithString:@"https://iisacc.com"];
            fixture.timeZone = [NSTimeZone timeZoneWithName:@"Asia/Seoul"];
            fixture.startDate = at("2026-10-02T00:00:00Z"); fixture.endDate = at("2026-10-02T01:00:00Z");
            fixture.occurrenceDate = fixture.startDate;
            fixture.creationDate = at("2026-10-01T00:00:00Z"); fixture.lastModifiedDate = fixture.creationDate;
            fixture.availability = EKEventAvailabilityFree; fixture.status = EKEventStatusTentative;
            auto event = convert(fixture, calendar);
            const CalendarRegistry registry;
            event.validate(registry);
            CHECK(event.title == "검토" && event.description == "설명" && event.location.name == "Seoul");
            CHECK(event.color == calendar.color && event.url == "https://iisacc.com");
            CHECK(event.range()->start == Instant::parse("2026-10-02T00:00:00Z"));
            CHECK(event.zone() == "Asia/Seoul" && !event.busy && event.status == EventStatus::Tentative);
            CHECK(event.created == Instant::parse("2026-10-01T00:00:00Z") && event.updated == event.created);
            CHECK(event.properties.at("external.source") == "icloud-source");
            CHECK(event.recurrence == RecurrenceRule{});
            fixture.alarms = @[(EKAlarm *)(id)[NSObject new]];
            const auto oldAlarms = fixture.alarms;
            EventOccurrenceUpdate change{"Edited", TimedSchedule{{Instant::parse("2026-10-02T02:00:00Z"), Instant::parse("2026-10-02T03:00:00Z")}, "Asia/Seoul"}};
            detail::applyAppleOccurrenceUpdate((EKEvent *)(id)fixture, event, change);
            CHECK([fixture.title isEqualToString:@"Edited"] && [fixture.notes isEqualToString:@"설명"] && fixture.alarms == oldAlarms);
            CHECK([fixture.startDate isEqualToDate:at("2026-10-02T02:00:00Z")]);
            fixture.title = @"검토"; fixture.startDate = at("2026-10-02T00:00:00Z"); fixture.endDate = at("2026-10-02T01:00:00Z");
            const auto firstId = event.id;
            fixture.startDate = at("2026-10-03T00:00:00Z"); fixture.endDate = at("2026-10-03T01:00:00Z");
            fixture.occurrenceDate = fixture.startDate;
            CHECK(convert(fixture, calendar).id != firstId);
            fixture.allDay = YES;
            NSTimeZone *previousZone = [NSTimeZone defaultTimeZone];
            [NSTimeZone setDefaultTimeZone:[NSTimeZone timeZoneWithName:@"America/New_York"]];
            // EventKit returns floating dates in the default zone, not item.timeZone.
            fixture.timeZone = [NSTimeZone timeZoneWithName:@"Asia/Seoul"];
            fixture.startDate = at("2026-11-01T04:00:00Z"); fixture.endDate = at("2026-11-02T05:00:00Z");
            fixture.occurrenceDate = fixture.startDate;
            event = convert(fixture, calendar); event.validate(registry);
            CHECK(std::get<AllDaySchedule>(event.schedule).start == Date(2026, 11, 1));
            CHECK(std::get<AllDaySchedule>(event.schedule).end == Date(2026, 11, 2));
            CHECK(event.range()->end - event.range()->start == iiCalendarIntegration::Duration(25 * 3600));
            CHECK(event.zone() == "America/New_York");
            // Some EventKit all-day records end later on their start date. A
            // positive same-day interval beginning at midnight covers that day.
            fixture.endDate = at("2026-11-02T03:00:00Z");
            event = convert(fixture, calendar); event.validate(registry);
            CHECK(std::get<AllDaySchedule>(event.schedule).end == Date(2026, 11, 2));
            CHECK(event.properties.at("external.apple.allDayEndNormalized") == "next-day");
            fixture.endDate = at("2026-11-01T03:00:00Z");
            event = convert(fixture, calendar);
            try { event.validate(registry); throw std::runtime_error("reversed all-day interval accepted"); }
            catch (const std::invalid_argument &) { CHECK(true); }
            [NSTimeZone setDefaultTimeZone:previousZone];
            fixture.allDay = NO; fixture.occurrenceDate = nil;
            fixture.startDate = [NSDate dateWithTimeIntervalSince1970:-0.000001];
            fixture.endDate = [NSDate dateWithTimeIntervalSince1970:1];
            event = convert(fixture, calendar); event.validate(registry);
            CHECK(event.range()->start == Instant(-1, AttosecondsPerSecond - 1'000'000'000'000ULL));
            fixture.status = EKEventStatusCanceled;
            CHECK(convert(fixture, calendar).status == EventStatus::Cancelled);
            fixture.calendarItemIdentifier = @"";
            try { (void)convert(fixture, calendar); throw std::runtime_error("missing identifier accepted"); }
            catch (const ExternalCalendarError &e) { CHECK(e.code() == ExternalCalendarErrorCode::InvalidResponse); }
            fixture.calendarItemIdentifier = @"event"; fixture.startDate = nil;
            try { (void)convert(fixture, calendar); throw std::runtime_error("missing date accepted"); }
            catch (const ExternalCalendarError &e) { CHECK(e.code() == ExternalCalendarErrorCode::InvalidResponse); }
            std::cout << "apple codec: " << checks << " checks passed\n";
        } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
    }
}
