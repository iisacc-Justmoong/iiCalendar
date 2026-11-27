#include "AppleEvent.h"
#import <EventKit/EventKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace iiCalendarIntegration {
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
void requireRemindersAccess() {
    const auto status = AppleCalendarProvider::remindersAuthorization();
    if (status == AppleCalendarAuthorization::NotDetermined)
        throw ExternalCalendarError(ExternalCalendarErrorCode::AuthenticationRequired, "Apple Reminders access has not been requested");
    if (status != AppleCalendarAuthorization::FullAccess)
        throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "Apple Reminders access is required");
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
detail::AppleTaskSourceMatch detail::classifyAppleTaskSource(const std::string &selectedId, const std::string &selectedName, int selectedType,
                                                           const std::string &candidateId, const std::string &candidateName, int candidateType) {
    if (selectedId.empty() || candidateId.empty()) return AppleTaskSourceMatch::None;
    if (selectedId == candidateId) return AppleTaskSourceMatch::Exact;
    if (!selectedName.empty() && selectedName == candidateName && selectedType == candidateType) return AppleTaskSourceMatch::Candidate;
    return AppleTaskSourceMatch::None;
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
        const auto localStart = timeZone.at(start).local;
        auto localEnd = timeZone.at(end).local.date;
        // Some EventKit records use a positive interval from midnight to later
        // on the same date for a one-day all-day event. Preserve that date in
        // the SDK's exclusive-end representation without modifying the source.
        if (localEnd == localStart.date && localStart.time == Time() && end > start) {
            localEnd = localEnd.addDays(1);
            event.properties["external.apple.allDayEndNormalized"] = "next-day";
        }
        event.schedule = AllDaySchedule{localStart.date, localEnd, zone};
    } else event.schedule = TimedSchedule{{start, end}, zone};
    if (item.creationDate) event.created = instant(item.creationDate);
    if (item.lastModifiedDate) event.updated = instant(item.lastModifiedDate);
    event.properties["external.calendarItemIdentifier"] = remoteId;
    event.properties["external.eventIdentifier"] = text(item.eventIdentifier);
    event.properties["external.occurrenceDate"] = occurrence.toString();
    event.properties["external.source"] = calendar.sourceId;
    return event;
}
void detail::applyAppleCalendarDraft(EKEvent *item, const Event &draft) {
    if (!item) throw std::invalid_argument("Apple Calendar event is required");
    if (draft.kind != EventKind::Event || draft.recurrence != RecurrenceRule{} || !draft.participants.empty() ||
        !draft.attachments.empty() || !draft.alarms.empty() || !draft.url.empty() || !draft.tags.empty() ||
        !draft.links.empty() || !draft.properties.empty() || !draft.location.address.empty() || !draft.location.uri.empty() ||
        draft.location.latitude || draft.location.longitude || draft.status != EventStatus::Confirmed || draft.priority ||
        draft.percentComplete || draft.due || draft.completed || draft.created != Instant{} || draft.updated != Instant{} ||
        !draft.color.empty() || draft.calendarSystem != "gregorian")
        throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "Apple Calendar event contains unsupported fields");
    draft.validate(CalendarRegistry());
    item.title = native(draft.title); item.notes = native(draft.description); item.location = native(draft.location.name);
    item.alarms = nil;
    item.availability = draft.busy ? EKEventAvailabilityBusy : EKEventAvailabilityFree;
    if (const auto *timed = std::get_if<TimedSchedule>(&draft.schedule)) {
        if (timed->range.start.fraction() % 1'000'000'000'000ULL || timed->range.end.fraction() % 1'000'000'000'000ULL)
            throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "Apple Calendar cannot represent this time precision");
        item.allDay = NO; item.timeZone = [NSTimeZone timeZoneWithName:native(timed->zone)];
        if (!item.timeZone) throw std::invalid_argument("Apple Calendar time zone is unavailable");
        item.startDate = date(timed->range.start); item.endDate = date(timed->range.end);
    } else if (const auto *allDay = std::get_if<AllDaySchedule>(&draft.schedule)) {
        const auto system = TimeZone::system();
        if (TimeZone(allDay->zone).id() != system.id())
            throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "Apple all-day events use the device time zone");
        item.allDay = YES; item.timeZone = [NSTimeZone defaultTimeZone];
        item.startDate = date(system.dayRange(allDay->start).start);
        item.endDate = date(system.dayRange(allDay->end).start);
    } else throw std::invalid_argument("event schedule is required");
}
void detail::applyAppleOccurrenceUpdate(EKEvent *item, const Event &original, const EventOccurrenceUpdate &change) {
    if (!item) throw std::invalid_argument("Apple Calendar event is required");
    auto candidate = original; candidate.title = change.title; candidate.schedule = change.schedule;
    if (change.schedule.index() != original.schedule.index())
        throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "changing all-day type is not supported");
    candidate.validate(CalendarRegistry());
    if (change.schedule != original.schedule) {
        if (const auto *timed = std::get_if<TimedSchedule>(&change.schedule)) {
            if (timed->range.start.fraction() % 1'000'000'000'000ULL || timed->range.end.fraction() % 1'000'000'000'000ULL)
                throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "Apple Calendar cannot represent this time precision");
            NSTimeZone *zone = [NSTimeZone timeZoneWithName:native(timed->zone)];
            if (!zone) throw std::invalid_argument("Apple Calendar time zone is unavailable");
            item.timeZone = zone; item.startDate = date(timed->range.start); item.endDate = date(timed->range.end);
        } else if (const auto *allDay = std::get_if<AllDaySchedule>(&change.schedule)) {
            const auto system = TimeZone::system();
            if (TimeZone(allDay->zone).id() != system.id())
                throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "Apple all-day events use the device time zone");
            item.startDate = date(system.dayRange(allDay->start).start); item.endDate = date(system.dayRange(allDay->end).start);
        } else throw std::invalid_argument("event schedule is required");
    }
    if (change.title != original.title) item.title = native(change.title);
}
void detail::prepareAppleCalendarDraft(EKEvent *item, EKCalendar *calendar, const Event &draft) {
    if (!item || !calendar) throw std::invalid_argument("Apple Calendar destination is required");
    item.calendar = calendar;
    applyAppleCalendarDraft(item, draft);
}
void detail::applyAppleReminderDraft(EKReminder *item, const Event &draft) {
    if (!item) throw std::invalid_argument("Apple reminder is required");
    if (draft.kind != EventKind::Task || draft.recurrence != RecurrenceRule{} || !draft.participants.empty() ||
        !draft.attachments.empty() || !draft.alarms.empty() || !draft.url.empty() || !draft.tags.empty() ||
        !draft.links.empty() || !draft.properties.empty() || !draft.location.name.empty() ||
        !draft.location.address.empty() || !draft.location.uri.empty() || draft.location.latitude || draft.location.longitude ||
        draft.status != EventStatus::Confirmed || draft.priority || draft.percentComplete || draft.due || draft.completed ||
        draft.created != Instant{} || draft.updated != Instant{} || !draft.color.empty() || draft.calendarSystem != "gregorian")
        throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "Apple reminder contains unsupported fields");
    draft.validate(CalendarRegistry());
    item.title = native(draft.title); item.notes = native(draft.description); item.alarms = nil;
    const auto zoneName = draft.zone();
    NSTimeZone *timeZone = [NSTimeZone timeZoneWithName:native(zoneName)];
    if (!timeZone) throw std::invalid_argument("Apple Reminders time zone is unavailable");
    NSCalendar *calendar = [[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian];
    calendar.timeZone = timeZone;
    auto dayComponents = [&](Date value) {
        NSDateComponents *parts = [NSDateComponents new];
        parts.calendar = calendar; parts.timeZone = timeZone;
        parts.year = value.year(); parts.month = value.month(); parts.day = value.day();
        return parts;
    };
    if (const auto *timed = std::get_if<TimedSchedule>(&draft.schedule)) {
        const TimeZone zone(zoneName);
        auto components = [&](Instant value) {
            if (value.fraction() != 0)
                throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "Apple Reminders requires whole-second times");
            const auto local = zone.at(value).local;
            if (zone.resolve(local, AmbiguousTime::Reject, MissingTime::Reject) != value)
                throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "Apple Reminders cannot represent this ambiguous time");
            NSDateComponents *parts = dayComponents(local.date);
            parts.hour = local.time.hour(); parts.minute = local.time.minute(); parts.second = local.time.second();
            return parts;
        };
        item.startDateComponents = components(timed->range.start);
        item.dueDateComponents = components(timed->range.end);
    } else if (const auto *allDay = std::get_if<AllDaySchedule>(&draft.schedule)) {
        item.startDateComponents = dayComponents(allDay->start);
        item.dueDateComponents = dayComponents(allDay->end.addDays(-1));
    } else throw std::invalid_argument("task schedule is required");
}
void detail::prepareAppleReminderDraft(EKReminder *item, EKCalendar *calendar, const Event &draft) {
    if (!item || !calendar) throw std::invalid_argument("Apple Reminders destination is required");
    item.calendar = calendar;
    applyAppleReminderDraft(item, draft);
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
AppleCalendarAuthorization AppleCalendarProvider::remindersAuthorization() {
    @autoreleasepool {
        const auto status = [EKEventStore authorizationStatusForEntityType:EKEntityTypeReminder];
        if (status == EKAuthorizationStatusNotDetermined) return AppleCalendarAuthorization::NotDetermined;
        if (status == EKAuthorizationStatusRestricted) return AppleCalendarAuthorization::Restricted;
        if (status == EKAuthorizationStatusDenied) return AppleCalendarAuthorization::Denied;
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
void AppleCalendarProvider::requestRemindersAccess(std::function<void(bool)> completion) {
    if (!completion) throw std::invalid_argument("Apple Reminders access callback required");
    std::lock_guard lock(impl_->mutex);
    @autoreleasepool {
        auto callback = ^(BOOL granted, NSError *) { completion(granted == YES); };
        if (@available(macOS 14.0, iOS 17.0, *))
            [impl_->store requestFullAccessToRemindersWithCompletion:callback];
        else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [impl_->store requestAccessToEntityType:EKEntityTypeReminder completion:callback];
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
                result.push_back({id, text(source.title), source.sourceType == EKSourceTypeCalDAV});
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
        if (!target || !impl_->matches(target) || !(target.allowedEntityTypes & EKEntityMaskEvent) ||
            (!calendar.sourceId.empty() && text(target.source.sourceIdentifier) != calendar.sourceId))
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
Event AppleCalendarProvider::createEvent(const RemoteCalendar &calendar, const Event &draft) {
    if (calendar.id.empty()) throw std::invalid_argument("calendar id is required");
    if (calendar.readOnly) throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "Apple Calendar is read-only");
    std::lock_guard lock(impl_->mutex);
    @autoreleasepool {
        requireAccess();
        [impl_->store reset];
        EKCalendar *target = [impl_->store calendarWithIdentifier:native(calendar.id)];
        if (!target || !impl_->matches(target) || !(target.allowedEntityTypes & EKEntityMaskEvent) ||
            (!calendar.sourceId.empty() && text(target.source.sourceIdentifier) != calendar.sourceId))
            throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "Apple Calendar is no longer connected");
        if (!target.allowsContentModifications)
            throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "Apple Calendar is read-only");
        EKEvent *item = [EKEvent eventWithEventStore:impl_->store];
        detail::prepareAppleCalendarDraft(item, target, draft);
        const auto availability = draft.busy ? EKEventAvailabilityBusy : EKEventAvailabilityFree;
        if (item.availability != availability)
            throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "Apple Calendar cannot preserve the requested availability");
        NSError *error = nil;
        if (![impl_->store saveEvent:item span:EKSpanThisEvent commit:YES error:&error])
            throw ExternalCalendarError(ExternalCalendarErrorCode::Transport, "Apple Calendar could not save the event");
        if (item.availability != availability || item.hasAlarms)
            throw ExternalCalendarError(ExternalCalendarErrorCode::InvalidResponse, "Apple Calendar returned unexpected event settings");
        return detail::appleCalendarEvent(item, calendar);
    }
}
Event AppleCalendarProvider::updateOccurrence(const RemoteCalendar &calendar, const Event &original, const EventOccurrenceUpdate &change) {
    if (calendar.id.empty() || original.id.empty() || !original.range()) throw std::invalid_argument("calendar and occurrence are required");
    if (calendar.readOnly) throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "Apple Calendar is read-only");
    std::lock_guard lock(impl_->mutex);
    @autoreleasepool {
        requireAccess(); [impl_->store reset];
        EKCalendar *target = [impl_->store calendarWithIdentifier:native(calendar.id)];
        if (!target || !impl_->matches(target) || !(target.allowedEntityTypes & EKEntityMaskEvent) ||
            (!calendar.sourceId.empty() && text(target.source.sourceIdentifier) != calendar.sourceId))
            throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "Apple Calendar is no longer connected");
        if (!target.allowsContentModifications) throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "Apple Calendar is read-only");
        // EventKit's identifier lookup returns the first occurrence of a series.
        // Query near this occurrence's current start and match its original ID.
        const auto start = original.range()->start;
        NSPredicate *predicate = [impl_->store predicateForEventsWithStartDate:date(Instant(start.seconds()) - Duration(1))
            endDate:date(Instant(start.seconds()) + Duration(1)) calendars:@[target]];
        EKEvent *matched = nil;
        for (EKEvent *item in [impl_->store eventsMatchingPredicate:predicate]) {
            auto current = detail::appleCalendarEvent(item, calendar);
            if (current.id != original.id) continue;
            if (matched) throw ExternalCalendarError(ExternalCalendarErrorCode::InvalidResponse, "ambiguous Apple Calendar occurrence");
            if (current.schedule != original.schedule || current.title != original.title ||
                (original.updated != Instant{} && current.updated != original.updated))
                throw ExternalCalendarError(ExternalCalendarErrorCode::Conflict, "Apple Calendar event changed");
            matched = item;
        }
        if (!matched) throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "Apple Calendar occurrence is unavailable");
        detail::applyAppleOccurrenceUpdate(matched, original, change);
        NSError *error = nil;
        if (![impl_->store saveEvent:matched span:EKSpanThisEvent commit:YES error:&error])
            throw ExternalCalendarError(ExternalCalendarErrorCode::Transport, "Apple Calendar could not save the occurrence");
        return detail::appleCalendarEvent(matched, calendar);
    }
}
std::vector<RemoteTaskList> AppleCalendarProvider::taskLists() {
    std::lock_guard lock(impl_->mutex);
    @autoreleasepool {
        requireRemindersAccess();
        [impl_->store reset];
        EKCalendar *defaultList = [impl_->store defaultCalendarForNewReminders];
        EKSource *selected = impl_->options.sourceId.empty() ? nil : [impl_->store sourceWithIdentifier:native(impl_->options.sourceId)];
        if (!impl_->options.sourceId.empty() && !selected)
            throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "Apple Calendar source is no longer connected");
        std::vector<RemoteTaskList> exact, candidates;
        for (EKCalendar *list in [impl_->store calendarsForEntityType:EKEntityTypeReminder]) {
            if (!list.allowsContentModifications) continue;
            const auto sourceId = text(list.source.sourceIdentifier), sourceName = text(list.source.title);
            const auto match = selected ? detail::classifyAppleTaskSource(impl_->options.sourceId, text(selected.title), static_cast<int>(selected.sourceType),
                sourceId, sourceName, static_cast<int>(list.source.sourceType)) : detail::AppleTaskSourceMatch::Exact;
            if (match == detail::AppleTaskSourceMatch::None) continue;
            if (exact.size() + candidates.size() >= impl_->options.maximumCalendars)
                throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Apple Reminders list limit exceeded");
            const bool unverified = match == detail::AppleTaskSourceMatch::Candidate;
            RemoteTaskList value{text(list.calendarIdentifier), text(list.title),
                !unverified && defaultList && [list.calendarIdentifier isEqualToString:defaultList.calendarIdentifier], sourceId, sourceName, unverified};
            (unverified ? candidates : exact).push_back(std::move(value));
        }
        requireRemindersAccess();
        return exact.empty() ? candidates : exact;
    }
}
Event AppleCalendarProvider::createTask(const RemoteTaskList &list, const Event &draft, bool confirmUnverifiedSource) {
    if (list.id.empty()) throw std::invalid_argument("reminder list id is required");
    if (list.requiresConfirmation && !confirmUnverifiedSource)
        throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "Apple Reminders source requires explicit confirmation");
    std::lock_guard lock(impl_->mutex);
    @autoreleasepool {
        requireRemindersAccess();
        [impl_->store reset];
        EKCalendar *target = [impl_->store calendarWithIdentifier:native(list.id)];
        if (!target || !(target.allowedEntityTypes & EKEntityMaskReminder))
            throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "Apple Reminders list is no longer connected");
        if (!impl_->matches(target)) {
            EKSource *selected = [impl_->store sourceWithIdentifier:native(impl_->options.sourceId)];
            if (!selected || !list.requiresConfirmation || list.sourceId != text(target.source.sourceIdentifier) ||
                detail::classifyAppleTaskSource(impl_->options.sourceId, text(selected.title), static_cast<int>(selected.sourceType),
                    text(target.source.sourceIdentifier), text(target.source.title), static_cast<int>(target.source.sourceType)) != detail::AppleTaskSourceMatch::Candidate)
                throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "Apple Reminders source is no longer available");
        }
        if (!target.allowsContentModifications)
            throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "Apple Reminders list is read-only");
        EKReminder *item = [EKReminder reminderWithEventStore:impl_->store];
        detail::prepareAppleReminderDraft(item, target, draft);
        NSError *error = nil;
        if (![impl_->store saveReminder:item commit:YES error:&error])
            throw ExternalCalendarError(ExternalCalendarErrorCode::Transport, "Apple Reminders could not save the task");
        Event saved = draft; saved.id = text(item.calendarItemIdentifier);
        if (saved.id.empty()) throw ExternalCalendarError(ExternalCalendarErrorCode::InvalidResponse, "Apple Reminders task identity unavailable");
        return saved;
    }
}
} // namespace iiCalendarIntegration
