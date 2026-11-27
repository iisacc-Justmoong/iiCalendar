#include <iiCalendarIntegration/iiCalendarIntegration.h>
#include <iostream>
#include <stdexcept>

using namespace iiCalendarIntegration;
namespace {
int checks = 0;
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) throw std::runtime_error(std::string(#__VA_ARGS__) + " at " + std::to_string(__LINE__)); } while (false)
template<class F> void fails(ExternalCalendarErrorCode code, F call) {
    try { call(); } catch (const ExternalCalendarError &error) { CHECK(error.code() == code); return; }
    throw std::runtime_error("expected task read error");
}

// Exercise two list pages, two task pages, undated tasks and date-only Google dates.
void google() {
    std::vector<CalendarHttpRequest> requests;
    GoogleCalendarProvider provider([] { return "fixture"; }, [&](const auto &request) {
        requests.push_back(request);
        if (request.url.find("/users/@me/lists") != std::string::npos) {
            if (request.url.find("pageToken=next") != std::string::npos)
                return CalendarHttpResponse{200, R"({"items":[{"id":"other","title":"Other"}]})"};
            return CalendarHttpResponse{200, R"({"items":[{"id":"one/a","title":"Home"}],"nextPageToken":"next"})"};
        }
        if (request.url.find("pageToken=next") != std::string::npos)
            return CalendarHttpResponse{200, R"({"items":[{"id":"two","title":"No date","status":"completed"}]})"};
        return CalendarHttpResponse{200, R"({"items":[{"id":"one","title":"Date only","notes":"note","status":"needsAction","due":"2026-11-25T00:00:00.000Z"}],"nextPageToken":"next"})"};
    });
    const auto lists = provider.allTaskLists();
    CHECK(lists.size() == 2 && lists[0].id == "one/a" && lists[1].title == "Other");
    const auto tasks = provider.tasks(lists[0]);
    CHECK(tasks.size() == 2 && tasks[0].dueDate == Date(2026, 11, 25));
    CHECK(!tasks[0].due && !tasks[0].start && tasks[0].description == "note");
    CHECK(tasks[1].completed && !tasks[1].dueDate && !tasks[1].due);
    CHECK(requests[2].url.find("one%2Fa/tasks") != std::string::npos);
    CHECK(requests[2].url.find("showHidden=true") != std::string::npos && requests[2].url.find("showAssigned=true") != std::string::npos);
}

// Graph task fields may be null and pagination must never send credentials elsewhere.
void microsoft() {
    std::vector<CalendarHttpRequest> requests;
    MicrosoftCalendarProvider provider([] { return "fixture"; }, [&](const auto &request) {
        requests.push_back(request);
        if (request.url.find("/tasks?") == std::string::npos)
            return CalendarHttpResponse{200, R"({"value":[{"id":"default","displayName":"Tasks","wellknownListName":"defaultList"},{"id":"x/y","displayName":"Other"}]})"};
        if (request.url.find("skiptoken=next") != std::string::npos)
            return CalendarHttpResponse{200, R"({"value":[{"id":"undated","title":"No date","status":"notStarted","dueDateTime":null}]})"};
        return CalendarHttpResponse{200, R"({"value":[{"id":"timed","title":"Timed","status":"completed","dueDateTime":{"dateTime":"2026-11-25T10:00:00","timeZone":"Korea Standard Time"}}],"@odata.nextLink":"https://graph.microsoft.com/v1.0/me/todo/lists/x%2Fy/tasks?$skiptoken=next"})"};
    });
    const auto lists = provider.allTaskLists();
    CHECK(lists.size() == 2 && lists[0].isDefault && !lists[1].isDefault);
    const auto tasks = provider.tasks(lists[1]);
    CHECK(tasks.size() == 2 && tasks[0].completed);
    CHECK(tasks[0].due == Instant::parse("2026-11-25T01:00:00Z") && !tasks[0].dueDate);
    CHECK(!tasks[1].due && !tasks[1].start);
    MicrosoftCalendarProvider foreign([] { return "fixture"; }, [](const auto &) {
        return CalendarHttpResponse{200, R"({"value":[],"@odata.nextLink":"https://evil.example/steal"})"};
    });
    fails(ExternalCalendarErrorCode::InvalidResponse, [&] { (void)foreign.tasks({"x", "", false}); });
}

// A list is account-scoped and its IDs cannot collide with another list's IDs.
class Fixture final : public ExternalCalendarProvider {
public:
    explicit Fixture(std::string provider = "fixture") : provider_(std::move(provider)) {}
    int listReads = 0;
    std::string id() const override { return provider_; }
    std::vector<RemoteCalendar> calendars() override { return {}; }
    std::vector<Event> events(const RemoteCalendar &, TimeRange) override { return {}; }
    std::vector<RemoteTaskList> allTaskLists() override { return {{"a", "A", true}, {"b", "B"}}; }
    std::vector<RemoteTaskList> taskLists() override { ++listReads; return {{"a", "A", true}}; }
    std::vector<RemoteTask> tasks(const RemoteTaskList &) override { RemoteTask task; task.id = "same"; task.title = "Test"; return {task}; }
private:
    std::string provider_;
};
void manager() {
    CalendarObjectManager manager;
    manager.login({"account", "fixture"}, std::make_shared<Fixture>());
    const auto tasks = manager.readTasks("account");
    CHECK(tasks.size() == 2 && tasks[0].id != tasks[1].id);
    CHECK(tasks[0].accountId == "account" && tasks[0].listId == "a" && tasks[1].listTitle == "B");
    manager.logout("account");
    fails(ExternalCalendarErrorCode::NotFound, [&] { (void)manager.readTasks("account"); });

    auto microsoft = std::make_shared<Fixture>("microsoft");
    manager.login({"ms-account", "microsoft"}, microsoft);
    (void)manager.readTasks("ms-account");
    const auto destinations = manager.taskLists("ms-account");
    CHECK(destinations.size() == 1 && destinations[0].id == "a" && microsoft->listReads == 0);
}
}
int main() {
    try { google(); microsoft(); manager(); std::cout << checks << " task reading checks passed\n"; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
