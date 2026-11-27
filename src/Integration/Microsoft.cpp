#include "Http.h"
#include "../Chronology/Icu.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <set>

namespace iiCalendarIntegration {
namespace {
using Json = nlohmann::json;
using Code = ExternalCalendarErrorCode;
const std::string endpoint = "https://graph.microsoft.com/v1.0";
constexpr std::size_t maximumBody = 16 * 1024 * 1024;
[[noreturn]] void fail(Code code, const char *message) { throw ExternalCalendarError(code, message); }
std::string encode(const std::string &value) {
    constexpr char hex[] = "0123456789ABCDEF"; std::string result;
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += char(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}
Json decode(const CalendarHttpResponse &response) {
    if (response.body.size() > maximumBody) fail(Code::LimitExceeded, "Microsoft Calendar response exceeds limit");
    return Json::parse(response.body, [](int depth, Json::parse_event_t, Json &) {
        if (depth > 32) throw std::invalid_argument("JSON depth"); return true;
    });
}
const Json &items(const Json &page) {
    if (!page.is_object() || page.contains("error") || !page.at("value").is_array()) throw std::invalid_argument("collection");
    return page.at("value");
}
std::string next(const Json &page, const std::string &path) {
    if (!page.contains("@odata.nextLink")) return {};
    const auto url = page.at("@odata.nextLink").get<std::string>();
    if (!url.empty() && (!url.starts_with(endpoint + path + '?') || url.find_first_of("\r\n\\#") != std::string::npos || url.find('\0') != std::string::npos))
        throw std::invalid_argument("untrusted pagination URL");
    return url;
}
std::string ianaZone(const std::string &zone) {
    try { return TimeZone(zone).id(); } catch (const std::exception &) {}
    const auto input = icu::UnicodeString::fromUTF8(zone); std::array<UChar, 256> output{}; UErrorCode status = U_ZERO_ERROR;
    const auto count = ucal_getTimeZoneIDForWindowsID(input.getBuffer(), input.length(), "001", output.data(), static_cast<int32_t>(output.size()), &status);
    detail::icuCheck(status);
    if (count <= 0 || count >= static_cast<int32_t>(output.size())) throw std::invalid_argument("unknown Graph time zone");
    return TimeZone(detail::utf8(icu::UnicodeString(output.data(), count))).id();
}
// Graph's all-day dates need a Windows time zone and local midnight boundaries.
std::string windowsZone(const std::string &zone) {
    const auto input = icu::UnicodeString::fromUTF8(TimeZone(zone).id());
    std::array<UChar, 256> output{}; UErrorCode status = U_ZERO_ERROR;
    const auto count = ucal_getWindowsTimeZoneID(input.getBuffer(), input.length(), output.data(), static_cast<int32_t>(output.size()), &status);
    detail::icuCheck(status);
    if (count <= 0 || count >= static_cast<int32_t>(output.size())) throw std::invalid_argument("unsupported Graph time zone");
    return detail::utf8(icu::UnicodeString(output.data(), count));
}
// Graph accepts at most seven fractional second digits; do not round user data.
std::string utcDateTime(Instant instant) {
    if (instant.fraction() % 100'000'000'000ULL != 0)
        throw ExternalCalendarError(Code::Unsupported, "Microsoft Calendar cannot represent this time precision");
    auto result = instant.toString(); result.pop_back(); return result;
}
// Preserve the authored zone while rejecting ambiguous wall times or mappings
// whose Windows-zone rules differ from the supplied IANA zone at this instant.
std::string graphLocalDateTime(Instant instant, const TimeZone &zone, const std::string &windows) {
    (void)utcDateTime(instant);
    const auto local = zone.at(instant).local;
    try {
        if (zone.resolve(local, AmbiguousTime::Reject, MissingTime::Reject) != instant ||
            TimeZone(ianaZone(windows)).at(instant).local != local)
            fail(Code::Unsupported, "Microsoft Calendar cannot preserve this time zone");
    } catch (const ExternalCalendarError &) { throw; }
    catch (...) { fail(Code::Unsupported, "Microsoft Calendar cannot preserve this time zone"); }
    return local.date.toString() + "T" + local.time.toString();
}
struct GraphTime { Instant instant; std::string zone; };
GraphTime time(const Json &value) {
    const auto dateTime = value.at("dateTime").get<std::string>();
    const auto separator = dateTime.find('T'); if (separator == std::string::npos) throw std::invalid_argument("dateTime");
    const auto zone = ianaZone(value.at("timeZone").get<std::string>());
    if (dateTime.find_first_of("Z+-", separator + 1) != std::string::npos) return {Instant::parse(dateTime), zone};
    return {TimeZone(zone).resolve({Date::parse(dateTime.substr(0, separator)), Time::parse(dateTime.substr(separator + 1))}), zone};
}
std::string text(const Json &value, const char *key) {
    return value.contains(key) && !value.at(key).is_null() ? value.at(key).get<std::string>() : std::string();
}
Event event(const Json &item, const RemoteCalendar &calendar) {
    Event result; result.id = item.at("id").get<std::string>(); if (result.id.empty()) throw std::invalid_argument("id");
    const auto type = item.value("type", std::string("singleInstance"));
    if (type != "singleInstance" && type != "occurrence" && type != "exception") throw std::invalid_argument("unexpanded recurrence master");
    result.calendarId = calendar.id; result.title = text(item, "subject"); result.url = text(item, "webLink"); result.color = calendar.color;
    result.description = text(item, "bodyPreview");
    if (item.contains("body") && item.at("body").is_object() && item.at("body").value("contentType", std::string()) == "text")
        result.description = text(item.at("body"), "content");
    const auto start = time(item.at("start")), end = time(item.at("end"));
    const auto originalZone = text(item, "originalStartTimeZone");
    const auto zone = originalZone.empty() ? start.zone : ianaZone(originalZone);
    if (item.value("isAllDay", false)) {
        const auto first = TimeZone(zone).at(start.instant).local, last = TimeZone(zone).at(end.instant).local;
        if (first.time != Time() || last.time != Time() || last.date <= first.date) throw std::invalid_argument("all-day boundaries");
        result.schedule = AllDaySchedule{first.date, last.date, zone};
    } else result.schedule = TimedSchedule{{start.instant, end.instant}, zone};
    const auto showAs = item.value("showAs", std::string("busy")); result.busy = showAs != "free";
    result.status = item.value("isCancelled", false) ? EventStatus::Cancelled : showAs == "tentative" ? EventStatus::Tentative : EventStatus::Confirmed;
    const auto sensitivity = item.value("sensitivity", std::string("normal"));
    result.visibility = sensitivity == "confidential" ? Visibility::Confidential : sensitivity == "normal" ? Visibility::Public : Visibility::Private;
    if (item.contains("createdDateTime")) result.created = Instant::parse(item.at("createdDateTime").get<std::string>());
    if (item.contains("lastModifiedDateTime")) result.updated = Instant::parse(item.at("lastModifiedDateTime").get<std::string>());
    if (item.contains("location") && item.at("location").is_object()) result.location.name = text(item.at("location"), "displayName");
    if (item.contains("categories")) result.tags = item.at("categories").get<std::vector<std::string>>();
    if (item.contains("attendees")) for (const auto &attendee : item.at("attendees")) {
        Participant person; const auto &address = attendee.at("emailAddress");
        person.address = text(address, "address"); person.id = person.address; person.name = text(address, "name");
        const auto role = attendee.value("type", std::string("required")); person.role = role == "optional" ? ParticipantRole::Optional : role == "resource" ? ParticipantRole::Resource : ParticipantRole::Required;
        const auto response = attendee.value("status", Json::object()).value("response", std::string());
        person.response = response == "accepted" ? Participation::Accepted : response == "tentativelyAccepted" ? Participation::Tentative
            : response == "declined" ? Participation::Declined : Participation::NeedsAction;
        result.participants.push_back(std::move(person));
    }
    if (item.contains("organizer") && item.at("organizer").contains("emailAddress")) {
        const auto &address = item.at("organizer").at("emailAddress");
        Participant person; person.address = text(address, "address"); person.id = person.address; person.name = text(address, "name"); person.role = ParticipantRole::Organizer;
        result.participants.push_back(std::move(person));
    }
    for (const auto *key : {"iCalUId", "seriesMasterId", "originalStartTimeZone", "originalEndTimeZone", "type", "@odata.etag"})
        if (item.contains(key) && item.at(key).is_string()) result.properties[std::string("microsoft.") + key] = item.at(key).get<std::string>();
    if (item.value("isReminderOn", false)) {
        const auto minutes = item.value("reminderMinutesBeforeStart", std::int64_t(0));
        if (minutes < 0 || minutes > 525600) throw std::invalid_argument("reminder");
        result.alarms.push_back({Duration(minutes * 60), "display", {}});
    }
    result.validate(CalendarRegistry()); return result;
}
template<class Work> auto objects(Work work) {
    try { return work(); }
    catch (const ExternalCalendarError &) { throw; }
    catch (...) { fail(Code::InvalidResponse, "invalid Microsoft Calendar response"); }
}
}
MicrosoftCalendarProvider::MicrosoftCalendarProvider(CalendarAccessToken token, CalendarHttpGet get, MicrosoftCalendarOptions options)
    : token_(std::move(token)), get_(get ? std::move(get) : detail::defaultCalendarHttpGet()), options_(options) {
    if (!token_ || !options_.maximumPages || !options_.maximumCalendars || !options_.maximumEvents)
        throw std::invalid_argument("Microsoft Calendar requires a token callback and positive limits");
}
CalendarHttpResponse MicrosoftCalendarProvider::request(const std::string &url, const std::string &method, const std::string &body, const std::string &etag) {
    std::string token;
    try { token = token_(); }
    catch (const ExternalCalendarError &e) { fail(e.code(), "Microsoft Calendar credential is unavailable"); }
    catch (...) { fail(Code::AuthenticationRequired, "Microsoft Calendar token is unavailable"); }
    if (token.empty() || token.size() > 16384 || token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~+/=") != std::string::npos)
        fail(Code::AuthenticationRequired, "Microsoft Calendar token is invalid");
    CalendarHttpResponse result;
    auto headers = std::map<std::string, std::string>{{"Authorization", "Bearer " + token}, {"Accept", "application/json"},
        {"Prefer", "outlook.timezone=\"UTC\", IdType=\"ImmutableId\""}};
    if (method == "POST" || method == "PATCH") headers.emplace("Content-Type", "application/json");
    if (!etag.empty()) {
        if (etag.size() > 4096 || etag.find_first_of("\r\n") != std::string::npos) throw std::invalid_argument("invalid calendar version");
        headers.emplace("If-Match", etag);
    }
    try { result = get_({url, std::move(headers), method, body}); }
    catch (...) { fail(Code::Transport, "Microsoft Calendar HTTPS request failed"); }
    if (result.status == 401) fail(Code::AuthenticationRequired, "Microsoft Calendar authentication is required");
    if (result.status == 403) fail(Code::PermissionDenied, "Microsoft Calendar read permission is required");
    if (result.status == 412) fail(Code::Conflict, "Microsoft Calendar event changed");
    if (result.status == 404) fail(Code::NotFound, "Microsoft Calendar was not found");
    if (result.status == 429) fail(Code::RateLimited, "Microsoft Calendar request was rate limited");
    if (result.status != 200 && !(method == "POST" && result.status == 201)) fail(Code::Transport, "Microsoft Calendar HTTPS request failed");
    return result;
}
Event MicrosoftCalendarProvider::createEvent(const RemoteCalendar &calendar, const Event &draft) {
    if (calendar.id.empty()) throw std::invalid_argument("calendar id is required");
    if (calendar.readOnly) fail(Code::PermissionDenied, "Microsoft Calendar is read-only");
    if (draft.kind != EventKind::Event || draft.recurrence != RecurrenceRule{} || !draft.participants.empty() ||
        !draft.attachments.empty() || !draft.alarms.empty() || !draft.url.empty() || !draft.tags.empty() ||
        !draft.links.empty() || !draft.properties.empty() || !draft.location.address.empty() || !draft.location.uri.empty() ||
        draft.location.latitude || draft.location.longitude || draft.status != EventStatus::Confirmed || draft.priority ||
        draft.percentComplete || draft.due || draft.completed || draft.created != Instant{} || draft.updated != Instant{} ||
        !draft.color.empty() || draft.calendarSystem != "gregorian")
        fail(Code::Unsupported, "Microsoft Calendar event contains unsupported fields");
    draft.validate(CalendarRegistry());
    Json body{{"subject", draft.title}, {"body", {{"contentType", "text"}, {"content", draft.description}}},
              {"location", {{"displayName", draft.location.name}}}, {"showAs", draft.busy ? "busy" : "free"},
              {"sensitivity", draft.visibility == Visibility::Public ? "normal" : draft.visibility == Visibility::Confidential ? "confidential" : "private"},
              {"isReminderOn", false}};
    if (const auto *timed = std::get_if<TimedSchedule>(&draft.schedule)) {
        const TimeZone zone(timed->zone); const auto windows = windowsZone(timed->zone);
        body["start"] = {{"dateTime", graphLocalDateTime(timed->range.start, zone, windows)}, {"timeZone", windows}};
        body["end"] = {{"dateTime", graphLocalDateTime(timed->range.end, zone, windows)}, {"timeZone", windows}};
        body["isAllDay"] = false;
    } else if (const auto *allDay = std::get_if<AllDaySchedule>(&draft.schedule)) {
        const auto zone = windowsZone(allDay->zone);
        body["start"] = {{"dateTime", allDay->start.toString() + "T00:00:00"}, {"timeZone", zone}};
        body["end"] = {{"dateTime", allDay->end.toString() + "T00:00:00"}, {"timeZone", zone}};
        body["isAllDay"] = true;
    } else throw std::invalid_argument("event schedule is required");
    const auto response = request(endpoint + "/me/calendars/" + encode(calendar.id) + "/events", "POST", body.dump());
    return objects([&] { return event(decode(response), calendar); });
}
Event MicrosoftCalendarProvider::updateOccurrence(const RemoteCalendar &calendar, const Event &original, const EventOccurrenceUpdate &change) {
    if (calendar.id.empty() || original.id.empty()) throw std::invalid_argument("calendar and occurrence are required");
    if (calendar.readOnly) fail(Code::PermissionDenied, "Microsoft Calendar is read-only");
    auto candidate = original; candidate.title = change.title; candidate.schedule = change.schedule;
    if (candidate.schedule.index() != original.schedule.index()) fail(Code::Unsupported, "changing all-day type is not supported");
    candidate.validate(CalendarRegistry());
    Json body = Json::object();
    if (change.title != original.title) body["subject"] = change.title;
    if (change.schedule != original.schedule) {
        if (const auto *timed = std::get_if<TimedSchedule>(&change.schedule)) {
            const TimeZone zone(timed->zone); const auto windows = windowsZone(timed->zone);
            body["start"] = {{"dateTime", graphLocalDateTime(timed->range.start, zone, windows)}, {"timeZone", windows}};
            body["end"] = {{"dateTime", graphLocalDateTime(timed->range.end, zone, windows)}, {"timeZone", windows}};
        } else if (const auto *allDay = std::get_if<AllDaySchedule>(&change.schedule)) {
            const auto zone = windowsZone(allDay->zone);
            body["start"] = {{"dateTime", allDay->start.toString() + "T00:00:00"}, {"timeZone", zone}};
            body["end"] = {{"dateTime", allDay->end.toString() + "T00:00:00"}, {"timeZone", zone}};
        } else throw std::invalid_argument("event schedule is required");
    }
    if (body.empty()) return original;
    const auto version = original.properties.find("microsoft.@odata.etag");
    const auto response = request(endpoint + "/me/calendars/" + encode(calendar.id) + "/events/" + encode(original.id),
                                  "PATCH", body.dump(), version == original.properties.end() ? "" : version->second);
    auto saved = objects([&] { return event(decode(response), calendar); });
    if (saved.id != original.id) fail(Code::InvalidResponse, "Microsoft Calendar returned a different occurrence");
    return saved;
}
std::vector<RemoteTaskList> MicrosoftCalendarProvider::taskLists() {
    auto result = allTaskLists();
    std::erase_if(result, [](const auto &list) { return !list.isDefault; });
    if (result.size() != 1) fail(Code::NotFound, "Microsoft default task list is unavailable");
    return result;
}
std::vector<RemoteTaskList> MicrosoftCalendarProvider::allTaskLists() {
    return objects([&] {
        const std::string path = "/me/todo/lists";
        auto url = endpoint + path + "?$select=id,displayName,wellknownListName&$top=200";
        std::set<std::string> pages, ids;
        std::vector<RemoteTaskList> result;
        for (std::size_t page = 0; !url.empty(); ++page) {
            if (page >= options_.maximumPages) fail(Code::LimitExceeded, "Microsoft task list page limit exceeded");
            if (!pages.insert(url).second) throw std::invalid_argument("repeated task list page");
            const auto data = decode(request(url));
            for (const auto &item : items(data)) {
                const auto id = text(item, "id");
                if (id.empty() || !ids.insert(id).second) throw std::invalid_argument("task list identity");
                if (ids.size() > options_.maximumCalendars) fail(Code::LimitExceeded, "Microsoft task list limit exceeded");
                result.push_back({id, text(item, "displayName"), item.value("wellknownListName", std::string()) == "defaultList"});
            }
            url = next(data, path);
        }
        if (std::count_if(result.begin(), result.end(), [](const auto &list) { return list.isDefault; }) > 1)
            throw std::invalid_argument("duplicate default task list");
        return result;
    });
}
std::vector<RemoteTask> MicrosoftCalendarProvider::tasks(const RemoteTaskList &list) {
    if (list.id.empty()) throw std::invalid_argument("task list id is required");
    return objects([&] {
        const auto path = "/me/todo/lists/" + encode(list.id) + "/tasks";
        auto url = endpoint + path + "?$top=200";
        std::set<std::string> pages, ids; std::vector<RemoteTask> result;
        for (std::size_t page = 0; !url.empty(); ++page) {
            if (page >= options_.maximumPages) fail(Code::LimitExceeded, "Microsoft task page limit exceeded");
            if (!pages.insert(url).second) throw std::invalid_argument("repeated task page");
            const auto data = decode(request(url));
            for (const auto &item : items(data)) {
                if (result.size() >= options_.maximumEvents) fail(Code::LimitExceeded, "Microsoft task count limit exceeded");
                RemoteTask task; task.id = text(item, "id");
                if (task.id.empty() || !ids.insert(task.id).second) throw std::invalid_argument("task identity");
                task.title = text(item, "title"); task.completed = text(item, "status") == "completed";
                if (item.contains("body") && item.at("body").is_object() && item.at("body").value("contentType", std::string()) == "text")
                    task.description = text(item.at("body"), "content");
                if (item.contains("startDateTime") && !item.at("startDateTime").is_null()) task.start = time(item.at("startDateTime")).instant;
                if (item.contains("dueDateTime") && !item.at("dueDateTime").is_null()) task.due = time(item.at("dueDateTime")).instant;
                result.push_back(std::move(task));
            }
            url = next(data, path);
        }
        return result;
    });
}
Event MicrosoftCalendarProvider::createTask(const RemoteTaskList &list, const Event &draft, bool) {
    if (list.id.empty() || !list.isDefault) fail(Code::NotFound, "Microsoft default task list is required");
    if (draft.kind != EventKind::Task || draft.recurrence != RecurrenceRule{} || !draft.participants.empty() ||
        !draft.attachments.empty() || !draft.alarms.empty() || !draft.url.empty() || !draft.tags.empty() ||
        !draft.links.empty() || !draft.properties.empty() || !draft.location.name.empty() ||
        !draft.location.address.empty() || !draft.location.uri.empty() || draft.location.latitude || draft.location.longitude ||
        draft.status != EventStatus::Confirmed || draft.priority || draft.percentComplete || draft.due || draft.completed ||
        draft.created != Instant{} || draft.updated != Instant{} || !draft.color.empty() || draft.calendarSystem != "gregorian")
        fail(Code::Unsupported, "Microsoft To Do task contains unsupported fields");
    draft.validate(CalendarRegistry());
    const auto *timed = std::get_if<TimedSchedule>(&draft.schedule);
    if (!timed) fail(Code::Unsupported, "Microsoft To Do date-only tasks are not supported yet");
    const TimeZone zone(timed->zone); const auto windows = windowsZone(timed->zone);
    const Json body{{"title", draft.title}, {"body", {{"contentType", "text"}, {"content", draft.description}}},
        {"startDateTime", {{"dateTime", graphLocalDateTime(timed->range.start, zone, windows)}, {"timeZone", windows}}},
        {"dueDateTime", {{"dateTime", graphLocalDateTime(timed->range.end, zone, windows)}, {"timeZone", windows}}}};
    const auto response = request(endpoint + "/me/todo/lists/" + encode(list.id) + "/tasks", "POST", body.dump());
    return objects([&] {
        const auto item = decode(response);
        Event saved; saved.id = item.at("id").get<std::string>(); saved.title = item.at("title").get<std::string>();
        if (saved.id.empty()) throw std::invalid_argument("task id");
        if (item.contains("body") && item.at("body").is_object() && item.at("body").value("contentType", std::string()) == "text")
            saved.description = text(item.at("body"), "content");
        saved.kind = EventKind::Task;
        saved.schedule = TimedSchedule{{time(item.at("startDateTime")).instant, time(item.at("dueDateTime")).instant}, timed->zone};
        saved.validate(CalendarRegistry());
        return saved;
    });
}
std::vector<RemoteCalendar> MicrosoftCalendarProvider::calendars() {
    return objects([&] {
        const std::string path = "/me/calendars"; std::string url = endpoint + path + "?$top=200";
        std::set<std::string> pages, ids; std::vector<RemoteCalendar> result;
        for (std::size_t page = 0; !url.empty(); ++page) {
            if (page >= options_.maximumPages) fail(Code::LimitExceeded, "Microsoft Calendar page limit exceeded");
            if (!pages.insert(url).second) throw std::invalid_argument("repeated page");
            const auto data = decode(request(url));
            for (const auto &item : items(data)) {
                if (result.size() == options_.maximumCalendars) fail(Code::LimitExceeded, "Microsoft Calendar count limit exceeded");
                RemoteCalendar value; value.id = item.at("id").get<std::string>(); value.title = text(item, "name");
                if (value.id.empty() || !ids.insert(value.id).second) throw std::invalid_argument("calendar identity");
                value.color = text(item, "hexColor"); if (value.color.empty()) value.color = text(item, "color");
                value.primary = item.value("isDefaultCalendar", false); value.readOnly = !item.value("canEdit", false);
                if (item.contains("owner") && item.at("owner").is_object()) { value.sourceId = text(item.at("owner"), "address"); value.sourceName = text(item.at("owner"), "name"); }
                result.push_back(std::move(value));
            }
            url = next(data, path);
        }
        return result;
    });
}
std::vector<Event> MicrosoftCalendarProvider::events(const RemoteCalendar &calendar, TimeRange window) {
    if (calendar.id.empty() || window.end <= window.start) throw std::invalid_argument("Microsoft Calendar requires a calendar and positive time window");
    return objects([&] {
        const auto path = "/me/calendars/" + encode(calendar.id) + "/calendarView";
        const auto end = window.end.fraction() ? Instant(window.end.seconds()) + Duration(1) : window.end;
        auto url = endpoint + path + "?startDateTime=" + encode(Instant(window.start.seconds()).toString())
            + "&endDateTime=" + encode(end.toString()) + "&$top=200";
        std::set<std::string> pages, ids; std::vector<Event> result; std::size_t total = 0;
        for (std::size_t page = 0; !url.empty(); ++page) {
            if (page >= options_.maximumPages) fail(Code::LimitExceeded, "Microsoft Calendar page limit exceeded");
            if (!pages.insert(url).second) throw std::invalid_argument("repeated page");
            const auto data = decode(request(url));
            for (const auto &item : items(data)) {
                if (++total > options_.maximumEvents) fail(Code::LimitExceeded, "Microsoft Calendar event limit exceeded");
                auto value = event(item, calendar);
                if (!ids.insert(value.id).second) throw std::invalid_argument("duplicate event");
                if (value.status != EventStatus::Cancelled && value.range()->overlaps(window)) result.push_back(std::move(value));
            }
            url = next(data, path);
        }
        return result;
    });
}
} // namespace iiCalendarIntegration
