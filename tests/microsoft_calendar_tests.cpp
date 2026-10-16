#include <iiCalendar/iiCalendar.h>
#include <nlohmann/json.hpp>
#include <iostream>

using namespace iiCalendar;
using Json = nlohmann::json;
namespace {
int checks = 0;
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) throw std::runtime_error(std::string(#__VA_ARGS__) + " at " + std::to_string(__LINE__)); } while (false)
template<class F> void fails(ExternalCalendarErrorCode code, F call) {
    try { call(); } catch (const ExternalCalendarError &e) { CHECK(e.code() == code); return; }
    throw std::runtime_error("expected Graph error");
}
CalendarHttpResponse response(const Json &value) { return {200, value.dump()}; }
Json event(std::string id = "event") {
    return {{"id", id}, {"subject", "Work"}, {"type", "occurrence"}, {"originalStartTimeZone", "Korea Standard Time"},
        {"start", {{"dateTime", "2026-10-02T00:00:00.1234567"}, {"timeZone", "UTC"}}},
        {"end", {{"dateTime", "2026-10-02T01:00:00.1234567"}, {"timeZone", "UTC"}}},
        {"bodyPreview", "plain text"}, {"showAs", "tentative"}, {"sensitivity", "private"},
        {"seriesMasterId", "series"}, {"categories", {"Work"}}, {"isReminderOn", true}, {"reminderMinutesBeforeStart", 15}};
}
void tests() {
    std::vector<CalendarHttpRequest> requests;
    auto get = [&](const CalendarHttpRequest &request) {
        requests.push_back(request);
        CHECK(request.headers.at("Authorization") == "Bearer fixture-token");
        CHECK(request.headers.at("Prefer").find("ImmutableId") != std::string::npos);
        if (request.url.find("calendarView") == std::string::npos) {
            if (request.url.find("skiptoken") != std::string::npos) return response({{"value", Json::array({{{"id", "second"}, {"name", "Personal"}}})}});
            return response({{"value", Json::array({{{"id", "a/b ?"}, {"name", "Work"}, {"isDefaultCalendar", true}, {"canEdit", true}}})},
                {"@odata.nextLink", "https://graph.microsoft.com/v1.0/me/calendars?$skiptoken=next"}});
        }
        auto allDay = event("all-day"); allDay["isAllDay"] = true;
        allDay["start"]["dateTime"] = "2026-10-01T15:00:00"; allDay["end"]["dateTime"] = "2026-10-02T15:00:00";
        auto cancelled = event("cancelled"); cancelled["isCancelled"] = true;
        return response({{"value", Json::array({event(), allDay, cancelled})}});
    };
    MicrosoftCalendarProvider provider([] { return "fixture-token"; }, get);
    const auto calendars = provider.calendars(); CHECK(calendars.size() == 2 && calendars[0].primary && !calendars[0].readOnly);
    const TimeRange window{Instant::parse("2026-10-01T00:00:00Z"), Instant::parse("2026-10-04T00:00:00Z")};
    const auto events = provider.events(calendars[0], window); CHECK(events.size() == 2);
    CHECK(requests.back().url.find("a%2Fb%20%3F/calendarView") != std::string::npos);
    CHECK(events[0].zone() == "Asia/Seoul" && events[0].status == EventStatus::Tentative);
    CHECK(events[0].range()->start.fraction() == 123456700000000000ULL);
    CHECK(events[0].alarms[0].beforeStart == Duration(900));
    CHECK(std::get<AllDaySchedule>(events[1].schedule).start == Date(2026, 10, 2));
    CHECK(std::get<AllDaySchedule>(events[1].schedule).end == Date(2026, 10, 3));
    for (const auto [status, code] : {std::pair{401, ExternalCalendarErrorCode::AuthenticationRequired}, {403, ExternalCalendarErrorCode::PermissionDenied},
             {404, ExternalCalendarErrorCode::NotFound}, {429, ExternalCalendarErrorCode::RateLimited}, {503, ExternalCalendarErrorCode::Transport}}) {
        MicrosoftCalendarProvider failing([] { return "fixture-token"; }, [=](auto &) { return CalendarHttpResponse{status, "private upstream failure"}; });
        fails(code, [&] { (void)failing.calendars(); });
    }
    unsigned calls = 0;
    MicrosoftCalendarProvider foreign([] { return "fixture-token"; }, [&](auto &) {
        ++calls; return response({{"value", Json::array()}, {"@odata.nextLink", "https://attacker.example/steal"}});
    });
    fails(ExternalCalendarErrorCode::InvalidResponse, [&] { (void)foreign.calendars(); }); CHECK(calls == 1);
    MicrosoftCalendarProvider repeated([] { return "fixture-token"; }, [&](const auto &request) {
        return response({{"value", Json::array()}, {"@odata.nextLink", request.url}});
    });
    fails(ExternalCalendarErrorCode::InvalidResponse, [&] { (void)repeated.calendars(); });
    MicrosoftCalendarProvider duplicate([] { return "fixture-token"; }, [&](auto &) {
        return response({{"value", Json::array({{{"id", "same"}}, {{"id", "same"}}})}});
    });
    fails(ExternalCalendarErrorCode::InvalidResponse, [&] { (void)duplicate.calendars(); });
    MicrosoftCalendarProvider limited([] { return "fixture-token"; }, get, {1, 1000, 10000});
    fails(ExternalCalendarErrorCode::LimitExceeded, [&] { (void)limited.calendars(); });
    MicrosoftCalendarProvider master([] { return "fixture-token"; }, [&](auto &) {
        auto value = event(); value["type"] = "seriesMaster"; return response({{"value", Json::array({value})}});
    });
    fails(ExternalCalendarErrorCode::InvalidResponse, [&] { (void)master.events(calendars[0], window); });
    MicrosoftCalendarProvider injected([] { return "token\r\nInjected: header"; }, get);
    fails(ExternalCalendarErrorCode::AuthenticationRequired, [&] { (void)injected.calendars(); });
    // Graph presents a 25-hour all-day span through UTC across US DST ending.
    MicrosoftCalendarProvider dst([] { return "fixture-token"; }, [&](auto &) {
        auto value = event(); value["isAllDay"] = true; value["originalStartTimeZone"] = "Pacific Standard Time";
        value["start"]["dateTime"] = "2026-11-01T07:00:00"; value["end"]["dateTime"] = "2026-11-02T08:00:00";
        return response({{"value", Json::array({value})}});
    });
    const auto fall = dst.events(calendars[0], {Instant::parse("2026-11-01T00:00:00Z"), Instant::parse("2026-11-03T00:00:00Z")});
    CHECK(fall.size() == 1 && fall[0].range()->end - fall[0].range()->start == Duration(25 * 3600));
}
}
int main() { try { tests(); std::cout << checks << " Microsoft Calendar checks passed\n"; } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; } }
