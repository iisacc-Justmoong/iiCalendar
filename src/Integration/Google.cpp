#include "Http.h"
#include <nlohmann/json.hpp>
#include <set>

namespace iiCalendarIntegration {
namespace {
using Json = nlohmann::json;
constexpr std::size_t maximumBody = 16 * 1024 * 1024;
const std::string endpoint = "https://www.googleapis.com/calendar/v3";
std::string encode(const std::string &value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') out += raw;
        else { out += '%'; out += digits[c >> 4]; out += digits[c & 15]; }
    }
    return out;
}
Json decode(const std::string &body) {
    return Json::parse(body, [](int depth, Json::parse_event_t, Json &) {
        if (depth > 32) throw std::invalid_argument("JSON nesting exceeds limit");
        return true;
    });
}
const Json &items(const Json &page) {
    if (!page.is_object()) throw std::invalid_argument("response must be an object");
    if (page.contains("error")) throw std::invalid_argument("unexpected error envelope");
    static const Json empty = Json::array();
    if (!page.contains("items")) return empty;
    const auto &value = page.at("items");
    if (!value.is_array()) throw std::invalid_argument("items must be an array");
    return value;
}
template<class F> auto responseObjects(F call) {
    try { return call(); }
    catch (const ExternalCalendarError &) { throw; }
    catch (const std::exception &) {
        // Parser/provider text may contain response data. Never expose it in errors.
        throw ExternalCalendarError(ExternalCalendarErrorCode::InvalidResponse, "invalid Google Calendar response");
    }
}
void nextPage(const std::string &token, std::set<std::string> &seen) {
    if (!token.empty() && !seen.insert(token).second)
        throw ExternalCalendarError(ExternalCalendarErrorCode::InvalidResponse, "repeated Google Calendar page token");
}
Instant eventTime(const Json &value, const std::string &fallbackZone) {
    const auto text = value.at("dateTime").get<std::string>();
    const auto t = text.find('T');
    if (t != std::string::npos && text.find_first_of("Z+-", t + 1) == std::string::npos) {
        const auto zone = value.value("timeZone", fallbackZone);
        return TimeZone(zone).resolve({Date::parse(text.substr(0, t)), Time::parse(text.substr(t + 1))});
    }
    return Instant::parse(text);
}
Event readEvent(const Json &item, const RemoteCalendar &calendar, const std::string &defaultZone) {
    Event result;
    result.id = item.at("id").get<std::string>();
    if (result.id.empty()) throw std::invalid_argument("event id is empty");
    result.calendarId = calendar.id;
    result.title = item.value("summary", "");
    result.description = item.value("description", "");
    result.url = item.value("htmlLink", "");
    result.location.name = item.value("location", "");
    result.color = calendar.color;
    result.busy = item.value("transparency", "opaque") != "transparent";
    const auto status = item.value("status", "confirmed");
    if (status == "tentative") result.status = EventStatus::Tentative;
    const auto visibility = item.value("visibility", "default");
    if (visibility == "public") result.visibility = Visibility::Public;
    else if (visibility == "confidential") result.visibility = Visibility::Confidential;
    const auto &start = item.at("start"), &end = item.at("end");
    const auto zone = start.value("timeZone", defaultZone);
    (void)TimeZone(zone);
    if (start.contains("date") && end.contains("date") && !start.contains("dateTime") && !end.contains("dateTime")) {
        result.schedule = AllDaySchedule{Date::parse(start.at("date").get<std::string>()),
                                        Date::parse(end.at("date").get<std::string>()), zone};
    } else if (start.contains("dateTime") && end.contains("dateTime") && !start.contains("date") && !end.contains("date")) {
        result.schedule = TimedSchedule{{eventTime(start, zone), eventTime(end, zone)}, zone};
    } else throw std::invalid_argument("mixed or missing event date fields");
    if (item.contains("created")) result.created = Instant::parse(item.at("created").get<std::string>());
    if (item.contains("updated")) result.updated = Instant::parse(item.at("updated").get<std::string>());
    for (const char *key : {"etag", "iCalUID", "recurringEventId", "colorId", "eventType"})
        if (item.contains(key)) result.properties[std::string("external.") + key] = item.at(key).get<std::string>();
    if (item.contains("originalStartTime")) result.properties["external.originalStartTime"] = item.at("originalStartTime").dump();
    if (item.contains("attendees")) {
        const auto &attendees = item.at("attendees");
        if (!attendees.is_array()) throw std::invalid_argument("attendees must be an array");
        for (const auto &person : attendees) {
            Participant participant;
            participant.id = person.value("id", person.value("email", ""));
            participant.name = person.value("displayName", "");
            const auto email = person.value("email", "");
            if (!email.empty()) participant.address = "mailto:" + email;
            if (person.value("organizer", false)) participant.role = ParticipantRole::Organizer;
            else if (person.value("resource", false)) participant.role = ParticipantRole::Resource;
            else if (person.value("optional", false)) participant.role = ParticipantRole::Optional;
            const auto response = person.value("responseStatus", "needsAction");
            if (response == "accepted") participant.response = Participation::Accepted;
            else if (response == "declined") participant.response = Participation::Declined;
            else if (response == "tentative") participant.response = Participation::Tentative;
            result.participants.push_back(std::move(participant));
        }
    }
    if (item.contains("attachments")) {
        const auto &attachments = item.at("attachments");
        if (!attachments.is_array()) throw std::invalid_argument("attachments must be an array");
        for (const auto &attachment : attachments)
            result.attachments.push_back({attachment.value("fileId", attachment.value("fileUrl", "")),
                                          attachment.value("title", ""), attachment.value("fileUrl", ""),
                                          attachment.value("mimeType", ""), 0});
    }
    return result;
}
}
GoogleCalendarProvider::GoogleCalendarProvider(CalendarAccessToken token, CalendarHttpGet get, GoogleCalendarOptions options)
    : token_(std::move(token)), get_(get ? std::move(get) : detail::defaultCalendarHttpGet()), options_(options) {
    if (!token_ || options.maximumPages == 0 || options.maximumCalendars == 0 || options.maximumEvents == 0)
        throw std::invalid_argument("Google Calendar requires a token supplier and positive limits");
}
CalendarHttpResponse GoogleCalendarProvider::request(const std::string &url, const std::string &method, const std::string &body, const std::string &etag) {
    std::string token;
    try { token = token_(); }
    catch (const ExternalCalendarError &e) { throw ExternalCalendarError(e.code(), "Google Calendar credential is unavailable"); }
    catch (...) { throw ExternalCalendarError(ExternalCalendarErrorCode::AuthenticationRequired, "Google Calendar token is unavailable"); }
    if (token.empty() || token.size() > 16384 || token.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~+/=") != std::string::npos)
        throw ExternalCalendarError(ExternalCalendarErrorCode::AuthenticationRequired, "Google Calendar token is invalid or expired");
    CalendarHttpResponse response;
    auto headers = std::map<std::string, std::string>{{"Authorization", "Bearer " + token}, {"Accept", "application/json"}};
    if (method == "POST" || method == "PATCH") headers.emplace("Content-Type", "application/json");
    if (!etag.empty()) {
        if (etag.size() > 4096 || etag.find_first_of("\r\n") != std::string::npos) throw std::invalid_argument("invalid calendar version");
        headers.emplace("If-Match", etag);
    }
    try { response = get_({url, std::move(headers), method, body}); }
    catch (const ExternalCalendarError &) { throw; }
    catch (...) { throw ExternalCalendarError(ExternalCalendarErrorCode::Transport, "Google Calendar request failed"); }
    if (response.body.size() > maximumBody)
        throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Google Calendar response exceeds size limit");
    if (response.status == 401)
        throw ExternalCalendarError(ExternalCalendarErrorCode::AuthenticationRequired, "Google Calendar login must be renewed");
    if (response.status == 429)
        throw ExternalCalendarError(ExternalCalendarErrorCode::RateLimited, "Google Calendar request quota exceeded");
    if (response.status == 403) {
        bool rateLimited = false;
        try {
            const auto error = decode(response.body).at("error");
            if (error.value("status", "") == "RESOURCE_EXHAUSTED") rateLimited = true;
            if (error.contains("errors") && error.at("errors").is_array())
                for (const auto &entry : error.at("errors")) {
                    const auto reason = entry.value("reason", "");
                    if (reason == "rateLimitExceeded" || reason == "userRateLimitExceeded" ||
                        reason == "quotaExceeded" || reason == "dailyLimitExceeded") rateLimited = true;
                }
        } catch (const std::exception &) {}
        if (rateLimited)
            throw ExternalCalendarError(ExternalCalendarErrorCode::RateLimited, "Google Calendar request quota exceeded");
        throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "Google Calendar read access is denied");
    }
    if (response.status == 412) throw ExternalCalendarError(ExternalCalendarErrorCode::Conflict, "Google Calendar event changed");
    if (response.status == 404)
        throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "Google Calendar was not found");
    if (response.status != 200 && !(method == "POST" && response.status == 201))
        throw ExternalCalendarError(ExternalCalendarErrorCode::Transport, "Google Calendar HTTP request was rejected");
    return response;
}
Event GoogleCalendarProvider::createEvent(const RemoteCalendar &calendar, const Event &draft) {
    if (calendar.id.empty()) throw std::invalid_argument("calendar id is required");
    if (calendar.readOnly) throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "Google Calendar is read-only");
    if (draft.kind != EventKind::Event || draft.recurrence != RecurrenceRule{} || !draft.participants.empty() ||
        !draft.attachments.empty() || !draft.alarms.empty() || !draft.url.empty() || !draft.tags.empty() ||
        !draft.links.empty() || !draft.properties.empty() || !draft.location.address.empty() || !draft.location.uri.empty() ||
        draft.location.latitude || draft.location.longitude || draft.status != EventStatus::Confirmed || draft.priority ||
        draft.percentComplete || draft.due || draft.completed || draft.created != Instant{} || draft.updated != Instant{} ||
        !draft.color.empty() || draft.calendarSystem != "gregorian")
        throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "Google Calendar event contains unsupported fields");
    draft.validate(CalendarRegistry());
    Json body{{"summary", draft.title}, {"description", draft.description}, {"location", draft.location.name},
              {"transparency", draft.busy ? "opaque" : "transparent"},
              {"visibility", draft.visibility == Visibility::Public ? "public" : draft.visibility == Visibility::Confidential ? "confidential" : "private"},
              {"reminders", {{"useDefault", false}}}};
    if (const auto *timed = std::get_if<TimedSchedule>(&draft.schedule)) {
        body["start"] = {{"dateTime", timed->range.start.toString()}, {"timeZone", timed->zone}};
        body["end"] = {{"dateTime", timed->range.end.toString()}, {"timeZone", timed->zone}};
    } else if (const auto *allDay = std::get_if<AllDaySchedule>(&draft.schedule)) {
        body["start"] = {{"date", allDay->start.toString()}};
        body["end"] = {{"date", allDay->end.toString()}};
    } else throw std::invalid_argument("event schedule is required");
    const auto response = request(endpoint + "/calendars/" + encode(calendar.id) + "/events", "POST", body.dump());
    return responseObjects([&] { return readEvent(decode(response.body), calendar, calendar.zone); });
}
Event GoogleCalendarProvider::updateOccurrence(const RemoteCalendar &calendar, const Event &original, const EventOccurrenceUpdate &change) {
    if (calendar.id.empty() || original.id.empty()) throw std::invalid_argument("calendar and occurrence are required");
    if (calendar.readOnly) throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "Google Calendar is read-only");
    auto candidate = original; candidate.title = change.title; candidate.schedule = change.schedule;
    if (candidate.schedule.index() != original.schedule.index()) throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "changing all-day type is not supported");
    candidate.validate(CalendarRegistry());
    Json body = Json::object();
    if (change.title != original.title) body["summary"] = change.title;
    if (change.schedule != original.schedule) {
        if (const auto *timed = std::get_if<TimedSchedule>(&change.schedule)) {
            body["start"] = {{"dateTime", timed->range.start.toString()}, {"timeZone", timed->zone}};
            body["end"] = {{"dateTime", timed->range.end.toString()}, {"timeZone", timed->zone}};
        } else if (const auto *allDay = std::get_if<AllDaySchedule>(&change.schedule)) {
            body["start"] = {{"date", allDay->start.toString()}}; body["end"] = {{"date", allDay->end.toString()}};
        } else throw std::invalid_argument("event schedule is required");
    }
    if (body.empty()) return original;
    const auto version = original.properties.find("external.etag");
    const auto response = request(endpoint + "/calendars/" + encode(calendar.id) + "/events/" + encode(original.id),
                                  "PATCH", body.dump(), version == original.properties.end() ? "" : version->second);
    auto saved = responseObjects([&] { return readEvent(decode(response.body), calendar, calendar.zone); });
    if (saved.id != original.id) throw ExternalCalendarError(ExternalCalendarErrorCode::InvalidResponse, "Google Calendar returned a different occurrence");
    return saved;
}
std::vector<RemoteTaskList> GoogleCalendarProvider::taskLists() {
    // Google's reserved list identifier resolves the authenticated user's default without a list request.
    return {{"@default", "Google Tasks", true}};
}
std::vector<RemoteTaskList> GoogleCalendarProvider::allTaskLists() {
    return responseObjects([&] {
        std::vector<RemoteTaskList> result;
        std::set<std::string> pages, ids;
        std::string token;
        for (std::size_t page = 0; page < options_.maximumPages; ++page) {
            auto url = std::string("https://tasks.googleapis.com/tasks/v1/users/@me/lists?maxResults=1000");
            if (!token.empty()) url += "&pageToken=" + encode(token);
            const auto data = decode(request(url).body);
            for (const auto &item : items(data)) {
                if (result.size() >= options_.maximumCalendars)
                    throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Google task list limit exceeded");
                const auto id = item.at("id").get<std::string>();
                if (id.empty() || !ids.insert(id).second) throw std::invalid_argument("task list identity");
                result.push_back({id, item.value("title", std::string()), false});
            }
            token = data.value("nextPageToken", std::string());
            if (token.empty()) return result;
            nextPage(token, pages);
        }
        throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Google task list page limit exceeded");
    });
}
std::vector<RemoteTask> GoogleCalendarProvider::tasks(const RemoteTaskList &list) {
    if (list.id.empty()) throw std::invalid_argument("task list id is required");
    return responseObjects([&] {
        std::vector<RemoteTask> result;
        std::set<std::string> pages, ids;
        std::string token; std::size_t count = 0;
        for (std::size_t page = 0; page < options_.maximumPages; ++page) {
            auto url = std::string("https://tasks.googleapis.com/tasks/v1/lists/") + encode(list.id)
                + "/tasks?maxResults=100&showCompleted=true&showHidden=true&showDeleted=false&showAssigned=true";
            if (!token.empty()) url += "&pageToken=" + encode(token);
            const auto data = decode(request(url).body);
            for (const auto &item : items(data)) {
                if (++count > options_.maximumEvents)
                    throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Google task count limit exceeded");
                if (item.value("deleted", false)) continue;
                RemoteTask task; task.id = item.at("id").get<std::string>();
                if (task.id.empty() || !ids.insert(task.id).second) throw std::invalid_argument("task identity");
                task.title = item.value("title", std::string()); task.description = item.value("notes", std::string());
                const auto status = item.value("status", std::string("needsAction"));
                if (status != "needsAction" && status != "completed") throw std::invalid_argument("task status");
                task.completed = status == "completed";
                if (item.contains("due") && !item.at("due").is_null()) {
                    const auto value = item.at("due").get<std::string>();
                    (void)Instant::parse(value);
                    task.dueDate = Date::parse(value.substr(0, 10));
                }
                result.push_back(std::move(task));
            }
            token = data.value("nextPageToken", std::string());
            if (token.empty()) return result;
            nextPage(token, pages);
        }
        throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Google task page limit exceeded");
    });
}
Event GoogleCalendarProvider::createTask(const RemoteTaskList &list, const Event &draft, bool) {
    if (list.id != "@default") throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "Google default task list is required");
    if (draft.kind != EventKind::Task || draft.recurrence != RecurrenceRule{} || !draft.participants.empty() ||
        !draft.attachments.empty() || !draft.alarms.empty() || !draft.url.empty() || !draft.tags.empty() ||
        !draft.links.empty() || !draft.properties.empty() || !draft.location.name.empty() ||
        !draft.location.address.empty() || !draft.location.uri.empty() || draft.location.latitude || draft.location.longitude ||
        draft.status != EventStatus::Confirmed || draft.priority || draft.percentComplete || draft.due || draft.completed ||
        draft.created != Instant{} || draft.updated != Instant{} || !draft.color.empty() || draft.calendarSystem != "gregorian")
        throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "Google Tasks supports only title, notes and a scheduled date here");
    draft.validate(CalendarRegistry());
    Date due;
    if (const auto *timed = std::get_if<TimedSchedule>(&draft.schedule)) due = TimeZone(timed->zone).at(timed->range.end).local.date;
    else if (const auto *allDay = std::get_if<AllDaySchedule>(&draft.schedule)) due = allDay->end.addDays(-1);
    else throw std::invalid_argument("task schedule is required");
    const Json body{{"title", draft.title}, {"notes", draft.description}, {"due", due.toString() + "T00:00:00Z"}};
    const auto response = request("https://tasks.googleapis.com/tasks/v1/lists/@default/tasks", "POST", body.dump());
    return responseObjects([&] {
        const auto item = decode(response.body);
        Event saved; saved.id = item.at("id").get<std::string>(); saved.title = item.at("title").get<std::string>();
        saved.description = item.value("notes", std::string{}); saved.kind = EventKind::Task;
        const auto value = item.at("due").get<std::string>();
        if (saved.id.empty() || value.size() < 10) throw std::invalid_argument("task response");
        const auto date = Date::parse(value.substr(0, 10));
        saved.schedule = AllDaySchedule{date, date.addDays(1), draft.zone()};
        saved.validate(CalendarRegistry());
        return saved;
    });
}
std::vector<RemoteCalendar> GoogleCalendarProvider::calendars() {
    return responseObjects([&] {
        std::vector<RemoteCalendar> result;
        std::set<std::string> pages, ids;
        std::string token;
        for (std::size_t pageIndex = 0; pageIndex < options_.maximumPages; ++pageIndex) {
            std::string url = endpoint + "/users/me/calendarList?maxResults=250&minAccessRole=reader&showHidden=true";
            if (!token.empty()) url += "&pageToken=" + encode(token);
            const auto page = decode(request(url).body);
            for (const auto &item : items(page)) {
                if (result.size() >= options_.maximumCalendars)
                    throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Google Calendar count limit exceeded");
                RemoteCalendar calendar;
                calendar.id = item.at("id").get<std::string>();
                if (calendar.id.empty() || !ids.insert(calendar.id).second) throw std::invalid_argument("duplicate or missing calendar id");
                calendar.title = item.value("summaryOverride", item.value("summary", ""));
                calendar.description = item.value("description", "");
                calendar.color = item.value("backgroundColor", "");
                calendar.zone = item.value("timeZone", "UTC");
                (void)TimeZone(calendar.zone);
                calendar.primary = item.value("primary", false);
                const auto role = item.value("accessRole", "reader");
                calendar.readOnly = role != "owner" && role != "writer" && role != "writerWithoutPrivateAccess";
                result.push_back(std::move(calendar));
            }
            token = page.value("nextPageToken", "");
            if (token.empty()) return result;
            nextPage(token, pages);
        }
        throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Google Calendar page limit exceeded");
    });
}
std::vector<Event> GoogleCalendarProvider::events(const RemoteCalendar &calendar, TimeRange window) {
    if (window.end <= window.start || calendar.id.empty()) throw std::invalid_argument("calendar and positive event window required");
    return responseObjects([&] {
        std::vector<Event> result;
        std::set<std::string> pages, ids;
        std::string token;
        std::size_t count = 0;
        const CalendarRegistry registry;
        // Google ignores fractional query seconds. Round outwards, then filter
        // the returned Event ranges against the caller's precise window.
        const auto queryStart = Instant(window.start.seconds());
        const auto queryEnd = window.end.fraction() == 0 ? window.end : Instant(window.end.seconds()) + Duration(1);
        for (std::size_t pageIndex = 0; pageIndex < options_.maximumPages; ++pageIndex) {
            std::string url = endpoint + "/calendars/" + encode(calendar.id) +
                "/events?singleEvents=true&showDeleted=false&orderBy=startTime&maxResults=2500&timeMin=" +
                encode(queryStart.toString()) + "&timeMax=" + encode(queryEnd.toString());
            if (!token.empty()) url += "&pageToken=" + encode(token);
            const auto page = decode(request(url).body);
            const auto zone = page.value("timeZone", calendar.zone);
            for (const auto &item : items(page)) {
                if (++count > options_.maximumEvents)
                    throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Google Calendar event limit exceeded");
                if (item.value("status", "confirmed") == "cancelled") continue;
                auto event = readEvent(item, calendar, zone);
                if (!ids.insert(event.id).second) throw std::invalid_argument("duplicate event id");
                event.validate(registry);
                if (event.range()->overlaps(window)) result.push_back(std::move(event));
            }
            token = page.value("nextPageToken", "");
            if (token.empty()) return result;
            nextPage(token, pages);
        }
        throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Google Calendar page limit exceeded");
    });
}
} // namespace iiCalendarIntegration
