#include "AppleEvent.h"
#import <EventKit/EventKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace iiCalendar {
namespace {
std::string text(NSString *value) {
    const char *utf8 = value ? value.UTF8String : nullptr;
    return utf8 ? std::string(utf8) : std::string{};
}
NSString *native(const std::string &value) {
    NSString *result = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
    if (!result) throw std::invalid_argument("Apple Calendar identifiers must be UTF-8");
    return result;
}
Instant instant(NSDate *date) {
    if (!date) throw ExternalCalendarError(ExternalCalendarErrorCode::InvalidResponse, "Apple Calendar event date missing");
    const double value = date.timeIntervalSince1970;
    // NSDate is floating point. Normalize to microseconds without claiming higher precision.
    if (!std::isfinite(value) || std::abs(value) > 8.64e12)
        throw ExternalCalendarError(ExternalCalendarErrorCode::InvalidResponse, "Apple Calendar date is out of range");
    const double whole = std::floor(value);
    auto seconds = static_cast<std::int64_t>(whole);
    auto micros = static_cast<std::uint64_t>(std::llround((value - whole) * 1'000'000));
    if (micros == 1'000'000) { ++seconds; micros = 0; }
    return Instant(seconds, micros * 1'000'000'000'000ULL);
}
NSDate *date(Instant value) {
    const double seconds = static_cast<double>(value.seconds()) + static_cast<double>(value.fraction()) / 1e18;
    if (std::abs(seconds) > 8.64e12) throw std::out_of_range("Apple Calendar query date is out of range");
    return [NSDate dateWithTimeIntervalSince1970:seconds];
}
void requireAccess() {
    const auto status = AppleCalendarProvider::authorization();
    if (status == AppleCalendarAuthorization::NotDetermined)
        throw ExternalCalendarError(ExternalCalendarErrorCode::AuthenticationRequired, "Apple Calendar full access has not been requested");
    if (status != AppleCalendarAuthorization::FullAccess)
        throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "Apple Calendar full read access is required");
}
std::string color(EKCalendar *calendar) {
    if (@available(macOS 10.15, iOS 4.0, *)) {
        CGColorRef original = calendar.CGColor;
        if (!original) return {};
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGColorRef converted = CGColorCreateCopyByMatchingToColorSpace(space, kCGRenderingIntentDefault, original, nullptr);
        CGColorSpaceRelease(space);
        if (!converted) return {};
        const CGFloat *components = CGColorGetComponents(converted);
        char result[8]{};
        if (CGColorGetNumberOfComponents(converted) >= 3) {
            auto channel = [](CGFloat value) { return static_cast<unsigned>(std::lround(std::clamp(static_cast<double>(value), 0.0, 1.0) * 255)); };
            std::snprintf(result, sizeof(result), "#%02X%02X%02X", channel(components[0]), channel(components[1]), channel(components[2]));
        }
        CGColorRelease(converted);
        return result;
    }
    return {};
}
}
Event detail::appleCalendarEvent(EKEvent *item, const RemoteCalendar &calendar) {
    Event event;
    const auto remoteId = text(item.calendarItemIdentifier);
    if (remoteId.empty()) throw ExternalCalendarError(ExternalCalendarErrorCode::InvalidResponse, "Apple Calendar event identifier missing");
    const auto start = instant(item.startDate), end = instant(item.endDate);
    const auto occurrence = item.occurrenceDate ? instant(item.occurrenceDate) : start;
    event.id = std::to_string(remoteId.size()) + ":" + remoteId + occurrence.toString();
    event.calendarId = calendar.id;
    event.title = text(item.title);
    event.description = text(item.notes);
    event.url = text(item.URL.absoluteString);
    event.color = calendar.color;
    event.location.name = text(item.location);
    event.busy = item.availability != EKEventAvailabilityFree;
    if (item.status == EKEventStatusTentative) event.status = EventStatus::Tentative;
    else if (item.status == EKEventStatusCanceled) event.status = EventStatus::Cancelled;
    // EKEvent.h specifies floating/all-day dates in the OS default time zone,
    // even when the event exposes another timeZone value.
    const auto zone = item.allDay || !item.timeZone ? text([NSTimeZone defaultTimeZone].name) : text(item.timeZone.name);
    if (item.allDay) {
        const TimeZone timeZone(zone);
        event.schedule = AllDaySchedule{timeZone.at(start).local.date, timeZone.at(end).local.date, zone};
    } else event.schedule = TimedSchedule{{start, end}, zone};
    if (item.creationDate) event.created = instant(item.creationDate);
    if (item.lastModifiedDate) event.updated = instant(item.lastModifiedDate);
    event.properties["external.calendarItemIdentifier"] = remoteId;
    event.properties["external.eventIdentifier"] = text(item.eventIdentifier);
    event.properties["external.occurrenceDate"] = occurrence.toString();
    event.properties["external.source"] = calendar.sourceId;
    return event;
}
struct AppleCalendarProvider::Impl {
    EKEventStore *store;
    AppleCalendarOptions options;
    std::mutex mutex;
    explicit Impl(AppleCalendarOptions value) : store([[EKEventStore alloc] init]), options(std::move(value)) {}
    bool matches(EKCalendar *calendar) const {
        return options.sourceId.empty() || text(calendar.source.sourceIdentifier) == options.sourceId;
    }
};
AppleCalendarProvider::AppleCalendarProvider(AppleCalendarOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {
    if (impl_->options.maximumCalendars == 0 || impl_->options.maximumEvents == 0)
        throw std::invalid_argument("Apple Calendar limits must be positive");
}
AppleCalendarProvider::~AppleCalendarProvider() = default;
bool AppleCalendarProvider::available() noexcept { return true; }
AppleCalendarAuthorization AppleCalendarProvider::authorization() {
    @autoreleasepool {
        const auto status = [EKEventStore authorizationStatusForEntityType:EKEntityTypeEvent];
        if (status == EKAuthorizationStatusNotDetermined) return AppleCalendarAuthorization::NotDetermined;
        if (status == EKAuthorizationStatusRestricted) return AppleCalendarAuthorization::Restricted;
        if (status == EKAuthorizationStatusDenied) return AppleCalendarAuthorization::Denied;
        if (@available(macOS 14.0, iOS 17.0, *)) {
            if (status == EKAuthorizationStatusWriteOnly) return AppleCalendarAuthorization::WriteOnly;
            if (status == EKAuthorizationStatusFullAccess) return AppleCalendarAuthorization::FullAccess;
        }
        // FullAccess and the legacy Authorized have the same OS enum value (3).
        if (static_cast<NSInteger>(status) == 3) return AppleCalendarAuthorization::FullAccess;
        return AppleCalendarAuthorization::Denied;
    }
}
void AppleCalendarProvider::requestAccess(std::function<void(bool)> completion) {
    if (!completion) throw std::invalid_argument("Apple Calendar access callback required");
    std::lock_guard lock(impl_->mutex);
    @autoreleasepool {
        auto callback = ^(BOOL granted, NSError *) { completion(granted == YES); };
        if (@available(macOS 14.0, iOS 17.0, *))
            [impl_->store requestFullAccessToEventsWithCompletion:callback];
        else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [impl_->store requestAccessToEntityType:EKEntityTypeEvent completion:callback];
#pragma clang diagnostic pop
        }
    }
}
std::vector<AppleCalendarSource> AppleCalendarProvider::sources() {
    std::lock_guard lock(impl_->mutex);
    @autoreleasepool {
        requireAccess();
        [impl_->store reset];
        std::vector<AppleCalendarSource> result;
        for (EKSource *source in impl_->store.sources) {
            const auto id = text(source.sourceIdentifier);
            if (impl_->options.sourceId.empty() || impl_->options.sourceId == id)
                result.push_back({id, text(source.title)});
        }
        requireAccess();
        return result;
    }
}
std::vector<RemoteCalendar> AppleCalendarProvider::calendars() {
    std::lock_guard lock(impl_->mutex);
    @autoreleasepool {
        requireAccess();
        [impl_->store reset];
        std::vector<RemoteCalendar> result;
        bool sourceFound = impl_->options.sourceId.empty();
        for (EKSource *source in impl_->store.sources)
            if (text(source.sourceIdentifier) == impl_->options.sourceId) sourceFound = true;
        if (!sourceFound) throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "Apple Calendar source is not connected");
        for (EKCalendar *calendar in [impl_->store calendarsForEntityType:EKEntityTypeEvent]) {
            if (!impl_->matches(calendar)) continue;
            if (result.size() >= impl_->options.maximumCalendars)
                throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Apple Calendar count limit exceeded");
            RemoteCalendar value;
            value.id = text(calendar.calendarIdentifier);
            value.title = text(calendar.title);
            value.color = color(calendar);
            value.zone = text([NSTimeZone defaultTimeZone].name);
            value.sourceId = text(calendar.source.sourceIdentifier);
            value.sourceName = text(calendar.source.title);
            value.primary = [calendar.calendarIdentifier isEqualToString:impl_->store.defaultCalendarForNewEvents.calendarIdentifier];
            value.readOnly = !calendar.allowsContentModifications;
            result.push_back(std::move(value));
        }
        requireAccess();
        return result;
    }
}
std::vector<Event> AppleCalendarProvider::events(const RemoteCalendar &calendar, TimeRange window) {
    if (window.end <= window.start) throw std::invalid_argument("event window must have positive duration");
    // EventKit silently truncates >4-year queries. A one-year limit is explicit.
    if (window.end - window.start > Duration(366 * 86400))
        throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Apple Calendar query window exceeds 366 days");
    std::lock_guard lock(impl_->mutex);
    @autoreleasepool {
        requireAccess();
        [impl_->store reset];
        EKCalendar *target = [impl_->store calendarWithIdentifier:native(calendar.id)];
        if (!target || !impl_->matches(target) || (!calendar.sourceId.empty() && text(target.source.sourceIdentifier) != calendar.sourceId))
            throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "Apple Calendar is no longer connected");
        // Expand outward by a second before NSDate conversion; filter exactly below.
        NSPredicate *predicate = [impl_->store predicateForEventsWithStartDate:date(Instant(window.start.seconds()) - Duration(1))
                                      endDate:date(Instant(window.end.seconds()) + Duration(1)) calendars:@[target]];
        std::vector<Event> result;
        std::set<std::string> ids;
        const CalendarRegistry registry;
        std::size_t count = 0;
        for (EKEvent *item in [impl_->store eventsMatchingPredicate:predicate]) {
            if (++count > impl_->options.maximumEvents)
                throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Apple Calendar event count limit exceeded");
            if (item.status == EKEventStatusCanceled) continue;
            auto event = detail::appleCalendarEvent(item, calendar);
            if (!ids.insert(event.id).second) throw ExternalCalendarError(ExternalCalendarErrorCode::InvalidResponse, "duplicate Apple Calendar occurrence");
            try { event.validate(registry); }
            catch (const std::exception &) { throw ExternalCalendarError(ExternalCalendarErrorCode::InvalidResponse, "invalid Apple Calendar event"); }
            if (event.range()->overlaps(window)) result.push_back(std::move(event));
        }
        requireAccess();
        return result;
    }
}
} // namespace iiCalendar
