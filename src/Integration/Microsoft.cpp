#include "Http.h"
#include "../Chronology/Icu.h"
#include <nlohmann/json.hpp>
#include <array>
#include <set>

namespace iiCalendar {
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
CalendarHttpResponse MicrosoftCalendarProvider::request(const std::string &url) {
    std::string token;
    try { token = token_(); }
    catch (const ExternalCalendarError &e) { fail(e.code(), "Microsoft Calendar credential is unavailable"); }
    catch (...) { fail(Code::AuthenticationRequired, "Microsoft Calendar token is unavailable"); }
    if (token.empty() || token.size() > 16384 || token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~+/=") != std::string::npos)
        fail(Code::AuthenticationRequired, "Microsoft Calendar token is invalid");
    CalendarHttpResponse result;
    try { result = get_({url, {{"Authorization", "Bearer " + token}, {"Accept", "application/json"},
        {"Prefer", "outlook.timezone=\"UTC\", IdType=\"ImmutableId\""}}}); }
    catch (...) { fail(Code::Transport, "Microsoft Calendar HTTPS request failed"); }
    if (result.status == 401) fail(Code::AuthenticationRequired, "Microsoft Calendar authentication is required");
    if (result.status == 403) fail(Code::PermissionDenied, "Microsoft Calendar read permission is required");
    if (result.status == 404) fail(Code::NotFound, "Microsoft Calendar was not found");
    if (result.status == 429) fail(Code::RateLimited, "Microsoft Calendar request was rate limited");
    if (result.status != 200) fail(Code::Transport, "Microsoft Calendar HTTPS request failed");
    return result;
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
} // namespace iiCalendar
