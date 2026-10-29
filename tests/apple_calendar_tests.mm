#include "../src/Integration/AppleEvent.h"
#include <iostream>

// An Objective-C selector fixture exercises the same native conversion as EKEvent,
// without creating an event store, reading personal data, or prompting for access.
@interface CalendarEventFixture : NSObject
@property(nonatomic, copy) NSString *calendarItemIdentifier, *eventIdentifier, *title, *notes, *location;
@property(nonatomic, copy) NSURL *URL;
@property(nonatomic, copy) NSDate *startDate, *endDate, *occurrenceDate, *creationDate, *lastModifiedDate;
@property(nonatomic, copy) NSTimeZone *timeZone;
@property(nonatomic, getter=isAllDay) BOOL allDay;
@property(nonatomic) EKEventAvailability availability;
@property(nonatomic) EKEventStatus status;
@end
@implementation CalendarEventFixture
@end

using namespace iiCalendar;
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
            RemoteCalendar calendar;
            calendar.id = "remote-calendar"; calendar.color = "#57965C"; calendar.sourceId = "icloud-source";
            CalendarEventFixture *fixture = [CalendarEventFixture new];
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
            CHECK(event.range()->end - event.range()->start == iiCalendar::Duration(25 * 3600));
            CHECK(event.zone() == "America/New_York");
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
