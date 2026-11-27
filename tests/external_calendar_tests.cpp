#include <iiCalendarIntegration/iiCalendarIntegration.h>
#include <condition_variable>
#include <future>
#include <iostream>
#include <set>

using namespace iiCalendarIntegration;
namespace {
int checks = 0;
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) throw std::runtime_error( \
    std::string(#__VA_ARGS__) + " at " + std::to_string(__LINE__)); } while (false)
template<class F> void fails(ExternalCalendarErrorCode code, F call) {
    try { call(); } catch (const ExternalCalendarError &e) { CHECK(e.code() == code); return; }
    throw std::runtime_error("expected external calendar error");
}
TimeRange window() {
    return {Instant::parse("2026-10-01T00:00:00Z"), Instant::parse("2026-11-01T00:00:00Z")};
}
Event event(std::string id = "same") {
    Event result;
    result.id = std::move(id);
    result.title = "외부 일정";
    result.schedule = TimedSchedule{{Instant::parse("2026-10-02T00:00:00Z"),
                                    Instant::parse("2026-10-02T01:00:00Z")}, "Asia/Seoul"};
    return result;
}
class FakeProvider : public ExternalCalendarProvider {
  public:
    std::vector<RemoteCalendar> list{{"same", "Calendar", {}, {}, "Asia/Seoul"}};
    std::vector<Event> entries{event()};
    std::optional<ExternalCalendarErrorCode> error;
    std::string id() const override { return "test"; }
    void maybeFail() { if (error) throw ExternalCalendarError(*error, "fixture failure"); }
    std::vector<RemoteCalendar> calendars() override { maybeFail(); return list; }
    std::vector<Event> events(const RemoteCalendar &, TimeRange) override {
        maybeFail(); return entries;
    }
};
// A write must use the selected account's existing calendar metadata, return
// the provider result, and update the loaded snapshot without another read.
void creationTests() {
    class Writable final : public FakeProvider {
    public:
        int reads = 0, writes = 0, listReads = 0, taskWrites = 0;
        std::string target;
        Writable() { list[0].readOnly = false; entries.clear(); }
        std::vector<RemoteCalendar> calendars() override { ++reads; return FakeProvider::calendars(); }
        std::vector<Event> events(const RemoteCalendar &calendar, TimeRange range) override {
            ++reads; return FakeProvider::events(calendar, range);
        }
        Event createEvent(const RemoteCalendar &calendar, const Event &draft) override {
            ++writes; target = calendar.id; maybeFail();
            auto saved = draft; saved.id = "saved"; return saved;
        }
        std::vector<RemoteTaskList> taskLists() override { ++listReads; return {{"list", "Tasks", true}, {"candidate", "Tasks", false, "other-source", "iCloud", true}}; }
        Event createTask(const RemoteTaskList &list, const Event &draft, bool) override {
            ++taskWrites; target = list.id; auto saved = draft; saved.id = "saved-task"; return saved;
        }
    };
    CalendarObjectManager manager;
    auto first = std::make_shared<Writable>(), second = std::make_shared<Writable>();
    manager.login({"a", "test"}, first, window());
    manager.login({"b", "test"}, second, window());
    const auto a = manager.calendars("a").at(0), b = manager.calendars("b").at(0);
    auto draft = event("draft"); draft.calendarId = b.id;
    const auto saved = manager.createEvent("b", b.id, draft);
    CHECK(first->writes == 0 && second->writes == 1 && second->target == b.remote.id);
    CHECK(second->reads == 2 && saved.calendarId == b.id && saved.ownerId == "b");
    CHECK(manager.calendar(b.id)->events.size() == 1 && manager.calendar(a.id)->events.empty());
    fails(ExternalCalendarErrorCode::NotFound, [&] { (void)manager.createEvent("a", b.id, draft); });
    CHECK(first->writes == 0 && second->writes == 1);
    const auto lists = manager.taskLists("b");
    CHECK(lists.size() == 2 && lists[0].isDefault && manager.taskLists("b").size() == 2 && second->listReads == 1);
    draft.kind = EventKind::Task;
    const auto task = manager.createTask("b", b.id, lists[0].id, draft);
    CHECK(task.kind == EventKind::Task && task.ownerId == "b" && first->taskWrites == 0 && second->taskWrites == 1);
    CHECK(second->target == "list" && manager.calendar(b.id)->events.size() == 1);
    fails(ExternalCalendarErrorCode::Unsupported, [&] { (void)manager.createTask("b", b.id, "candidate", draft); });
    CHECK(second->taskWrites == 1);
    (void)manager.createTask("b", b.id, "candidate", draft, true);
    CHECK(second->taskWrites == 2 && second->target == "candidate");
    fails(ExternalCalendarErrorCode::NotFound, [&] { (void)manager.createTask("b", b.id, "other", draft); });
    CHECK(second->taskWrites == 2);
    draft.kind = EventKind::Event; second->error = ExternalCalendarErrorCode::PermissionDenied;
    fails(ExternalCalendarErrorCode::PermissionDenied, [&] { (void)manager.createEvent("b", b.id, draft); });
    CHECK(manager.calendar(b.id)->events.size() == 1 && manager.accounts().at(1).state == ExternalAccountState::Ready);
    CalendarObjectManager limited({1000, 10000, 1});
    auto full = std::make_shared<Writable>(); full->entries = {event("existing")};
    limited.login({"limited", "test"}, full, window());
    const auto fullCalendar = limited.calendars("limited").at(0);
    draft.calendarId = fullCalendar.id;
    fails(ExternalCalendarErrorCode::LimitExceeded, [&] { (void)limited.createEvent("limited", fullCalendar.id, draft); });
    CHECK(full->writes == 0 && limited.calendar(fullCalendar.id)->events.size() == 1);
}
// Editing targets exactly the selected occurrence and publishes only a confirmed result.
void occurrenceUpdateTests() {
    class Writable final : public FakeProvider {
    public:
        int writes = 0; bool ignoreChanges = false;
        Writable() { list[0].readOnly = false; entries = {event("first"), event("second")}; }
        Event updateOccurrence(const RemoteCalendar &, const Event &original, const EventOccurrenceUpdate &change) override {
            ++writes; maybeFail();
            auto saved = original;
            if (!ignoreChanges) { saved.title = change.title; saved.schedule = change.schedule; }
            return saved;
        }
    };
    CalendarObjectManager manager;
    auto provider = std::make_shared<Writable>();
    manager.login({"owner", "test"}, provider, window());
    auto object = manager.calendars("owner").at(0);
    const auto original = object.events.at(0);
    EventOccurrenceUpdate change{"Changed", TimedSchedule{{Instant::parse("2026-10-03T00:00:00Z"),
        Instant::parse("2026-10-03T01:00:00Z")}, "Asia/Seoul"}};
    provider->error = ExternalCalendarErrorCode::Transport;
    fails(*provider->error, [&] { manager.updateOccurrence("owner", object.id, original.id, change); });
    CHECK(manager.calendar(object.id)->events == object.events);
    provider->error.reset(); provider->ignoreChanges = true;
    fails(ExternalCalendarErrorCode::InvalidResponse, [&] { manager.updateOccurrence("owner", object.id, original.id, change); });
    CHECK(manager.calendar(object.id)->events == object.events);
    provider->ignoreChanges = false;
    const auto saved = manager.updateOccurrence("owner", object.id, original.id, change);
    CHECK(saved.id == original.id && saved.title == "Changed");
    auto updated = manager.calendar(object.id).value();
    CHECK(updated.events.size() == 2 && updated.events.at(1).title == "Changed");
    CHECK(updated.events.at(0).id != saved.id && updated.events.at(0).title == original.title);
    fails(ExternalCalendarErrorCode::NotFound, [&] { manager.updateOccurrence("other", object.id, original.id, change); });
    auto readOnly = std::make_shared<FakeProvider>();
    manager.login({"reader", "test"}, readOnly, window());
    const auto locked = manager.calendars("reader").at(0);
    fails(ExternalCalendarErrorCode::PermissionDenied, [&] { manager.updateOccurrence("reader", locked.id, locked.events.at(0).id, change); });
    CHECK(provider->writes == 3);
}
void managerTests() {
    occurrenceUpdateTests();
    creationTests();
    CalendarObjectManager manager;
    auto a = std::make_shared<FakeProvider>(), b = std::make_shared<FakeProvider>();
    manager.login({"a", "test", "A"}, a);
    manager.login({"b", "test", "B"}, b);
    auto ca = manager.calendars("a").at(0), cb = manager.calendars("b").at(0);
    CHECK(ca.id != cb.id && ca.remote.id == cb.remote.id);
    CHECK(manager.accounts().size() == 2);
    manager.loadEvents("a", ca.id, window());
    manager.loadEvents("b", cb.id, window());
    ca = manager.calendar(ca.id).value(); cb = manager.calendar(cb.id).value();
    CHECK(ca.events.size() == 1 && ca.loadedWindow == window());
    CHECK(ca.events[0].id != cb.events[0].id);
    CHECK(ca.events[0].calendarId == ca.id && ca.events[0].ownerId == "a");
    auto store = Store::inMemory();
    store.put(ca.events[0]); store.put(cb.events[0]);
    CHECK(store.query().size() == 2);
    CHECK(dayDetails(store, Date(2026, 10, 2), TimeZone("Asia/Seoul")).events.total == 2);
    fails(ExternalCalendarErrorCode::NotFound, [&] { manager.loadEvents("a", cb.id, window()); });
    a->entries = {event("new")};
    manager.loadEvents("a", ca.id, window());
    CHECK(manager.calendar(ca.id)->events.size() == 1);
    CHECK(manager.calendar(ca.id)->events[0].id != ca.events[0].id);
    ca = manager.calendar(ca.id).value();
    a->error = ExternalCalendarErrorCode::Transport;
    fails(*a->error, [&] { manager.loadEvents("a", ca.id, window()); });
    CHECK(manager.calendar(ca.id)->events == ca.events);
    CHECK(manager.accounts().at(0).state == ExternalAccountState::Failed);
    a->error.reset();
    a->entries = {event("duplicate"), event("duplicate")};
    fails(ExternalCalendarErrorCode::InvalidResponse, [&] { manager.loadEvents("a", ca.id, window()); });
    CHECK(manager.calendar(ca.id)->events == ca.events);
    a->entries = {event()};
    a->error = ExternalCalendarErrorCode::AuthenticationRequired;
    fails(*a->error, [&] { manager.refresh("a"); });
    CHECK(manager.calendars("a").empty());
    CHECK(manager.accounts().at(0).state == ExternalAccountState::AuthenticationRequired);
    a->error.reset();
    manager.login({"a", "test", "A"}, a);
    CHECK(manager.calendars("a").size() == 1);
    a->list.clear();
    manager.refresh("a");
    CHECK(manager.calendars("a").empty());
    CHECK(manager.logout("a") && !manager.logout("a"));
    CHECK(manager.calendars().size() == 1);
    fails(ExternalCalendarErrorCode::NotFound, [&] { manager.refresh("a"); });
    CalendarObjectManager limited({1, 1});
    a->list = {{"one"}, {"two"}};
    fails(ExternalCalendarErrorCode::LimitExceeded, [&] { limited.login({"a", "test"}, a); });
    CHECK(limited.calendars().empty());
    a->list = {{"one"}};
    limited.login({"a", "test"}, a);
    a->entries = {event("1"), event("2")};
    fails(ExternalCalendarErrorCode::LimitExceeded, [&] {
        limited.loadEvents("a", limited.calendars()[0].id, window());
    });
    a->entries = {event()};
    a->entries[0].recurrence.frequency = Frequency::Daily;
    fails(ExternalCalendarErrorCode::InvalidResponse, [&] {
        limited.loadEvents("a", limited.calendars()[0].id, window());
    });
    // A completion for a logged-out account must not populate a later login.
    class BlockingProvider final : public FakeProvider {
      public:
        std::mutex mutex; std::condition_variable cv; bool entered = false, release = false;
        std::vector<Event> events(const RemoteCalendar &, TimeRange) override {
            std::unique_lock lock(mutex); entered = true; cv.notify_all();
            cv.wait(lock, [&] { return release; }); return entries;
        }
    };
    auto slow = std::make_shared<BlockingProvider>();
    manager.login({"slow", "test"}, slow);
    const auto slowId = manager.calendars("slow")[0].id;
    auto pending = std::async(std::launch::async, [&] {
        try { manager.loadEvents("slow", slowId, window()); }
        catch (const ExternalCalendarError &e) { return e.code(); }
        return ExternalCalendarErrorCode::Unsupported;
    });
    { std::unique_lock lock(slow->mutex); slow->cv.wait(lock, [&] { return slow->entered; }); }
    fails(ExternalCalendarErrorCode::Busy, [&] { manager.refresh("slow"); });
    manager.logout("slow");
    manager.login({"slow", "test"}, std::make_shared<FakeProvider>());
    { std::lock_guard lock(slow->mutex); slow->release = true; slow->cv.notify_all(); }
    CHECK(pending.get() == ExternalCalendarErrorCode::StaleSession);
    CHECK(manager.calendar(slowId)->events.empty());
}
void googleTests() {
    std::vector<CalendarHttpRequest> requests;
    std::vector<CalendarHttpResponse> responses;
    std::size_t cursor = 0, tokens = 0;
    auto get = [&](const CalendarHttpRequest &request) {
        requests.push_back(request); return responses.at(cursor++);
    };
    GoogleCalendarProvider provider([&] { ++tokens; return "fixture-token"; }, get);
    responses = {{200, R"({"items":[{"id":"a/b@example.com","summary":"업무", "timeZone":"Asia/Seoul","accessRole":"owner","primary":true}],"nextPageToken":"p/+"})"},
                 {200, R"({"items":[{"id":"read","summary":"Read","accessRole":"reader"}]})"}};
    const auto calendars = provider.calendars();
    CHECK(calendars.size() == 2 && calendars[0].primary && !calendars[0].readOnly);
    CHECK(calendars[1].readOnly && calendars[1].zone == "UTC");
    CHECK(requests[1].url.find("pageToken=p%2F%2B") != std::string::npos);
    CHECK(tokens == 2 && requests[0].headers.at("Authorization") == "Bearer fixture-token");
    cursor = 0; requests.clear();
    responses = {{200, R"({"id":"created","summary":"Review","description":"Notes","location":"Studio","start":{"dateTime":"2026-10-02T00:00:00Z","timeZone":"Asia/Seoul"},"end":{"dateTime":"2026-10-02T01:00:00Z","timeZone":"Asia/Seoul"}})"}};
    auto draft = event("draft"); draft.title = "Review"; draft.description = "Notes"; draft.location.name = "Studio";
    auto created = provider.createEvent(calendars[0], draft);
    CHECK(requests.size() == 1 && requests[0].method == "POST");
    CHECK(requests[0].url == "https://www.googleapis.com/calendar/v3/calendars/a%2Fb%40example.com/events");
    CHECK(requests[0].body.find("\"summary\":\"Review\"") != std::string::npos);
    CHECK(requests[0].body.find("\"timeZone\":\"Asia/Seoul\"") != std::string::npos);
    CHECK(created.id == "created" && created.title == "Review" && created.location.name == "Studio");
    // A recurring instance is patched by its occurrence ID, preserving all other fields.
    cursor = 0; requests.clear();
    responses = {{200, R"({"id":"instance/2","summary":"Updated","recurringEventId":"series","etag":"v2","start":{"dateTime":"2026-10-03T00:00:00Z"},"end":{"dateTime":"2026-10-03T01:00:00Z"}})"}};
    auto original = event("instance/2"); original.properties["external.etag"] = "v1";
    EventOccurrenceUpdate change{"Updated", TimedSchedule{{Instant::parse("2026-10-03T00:00:00Z"), Instant::parse("2026-10-03T01:00:00Z")}, "UTC"}};
    CHECK(provider.updateOccurrence(calendars[0], original, change).title == "Updated");
    CHECK(requests[0].method == "PATCH" && requests[0].url.ends_with("/events/instance%2F2"));
    CHECK(requests[0].headers.at("If-Match") == "v1");
    CHECK(requests[0].body.find("recurrence") == std::string::npos && requests[0].body.find("attendees") == std::string::npos);
    cursor = 0; responses = {{412, ""}};
    fails(ExternalCalendarErrorCode::Conflict, [&] { provider.updateOccurrence(calendars[0], original, change); });
    const auto count = requests.size();
    fails(ExternalCalendarErrorCode::PermissionDenied, [&] { (void)provider.createEvent(calendars[1], draft); });
    CHECK(requests.size() == count);
    draft.alarms.push_back({Duration(600)});
    fails(ExternalCalendarErrorCode::Unsupported, [&] { (void)provider.createEvent(calendars[0], draft); });
    CHECK(requests.size() == count); draft.alarms.clear();
    const auto lists = provider.taskLists();
    CHECK(lists.size() == 1 && lists[0].isDefault && requests.size() == count);
    draft.kind = EventKind::Task; draft.location.name.clear();
    cursor = 0; requests.clear();
    responses = {{200, R"({"id":"task-id","title":"Review","notes":"Notes","due":"2026-10-02T00:00:00.000Z"})"}};
    const auto task = provider.createTask(lists[0], draft);
    CHECK(requests.size() == 1 && requests[0].method == "POST");
    CHECK(requests[0].url == "https://tasks.googleapis.com/tasks/v1/lists/@default/tasks");
    CHECK(requests[0].body.find("2026-10-02T00:00:00Z") != std::string::npos);
    CHECK(task.kind == EventKind::Task && task.id == "task-id" && std::holds_alternative<AllDaySchedule>(task.schedule));
    responses = {{200, R"({"timeZone":"Asia/Seoul","items":[{"id":"timed","summary":"Review","start":{"dateTime":"2026-10-02T09:00:00+09:00"},"end":{"dateTime":"2026-10-02T10:00:00+09:00"},"transparency":"transparent","visibility":"public","updated":"2026-10-01T00:00:00Z","recurringEventId":"series","attendees":[{"email":"a@example.test","responseStatus":"accepted"}]},{"id":"cancelled","status":"cancelled"}],"nextPageToken":"next"})"},
                 {200, R"({"items":[{"id":"day","start":{"date":"2026-10-03"},"end":{"date":"2026-10-04"}}]})"}};
    cursor = 0; requests.clear();
    const auto events = provider.events(calendars[0], window());
    CHECK(events.size() == 2 && events[0].title == "Review");
    CHECK(events[0].range()->start == Instant::parse("2026-10-02T00:00:00Z"));
    CHECK(!events[0].busy && events[0].visibility == Visibility::Public);
    CHECK(events[0].participants[0].response == Participation::Accepted);
    CHECK(events[0].recurrence.frequency == Frequency::None);
    CHECK(events[0].properties.at("external.recurringEventId") == "series");
    CHECK(std::get<AllDaySchedule>(events[1].schedule).end == Date(2026, 10, 4));
    CHECK(events[1].zone() == "Asia/Seoul");
    CHECK(requests[0].url.find("a%2Fb%40example.com/events") != std::string::npos);
    CHECK(requests[0].url.find("singleEvents=true") != std::string::npos);
    CHECK(requests[0].url.find("timeMin=2026-10-01T00%3A00%3A00Z") != std::string::npos);
    CHECK(requests[1].url.find("pageToken=next") != std::string::npos);
    auto failure = [&](CalendarHttpResponse response, ExternalCalendarErrorCode code) {
        cursor = 0; responses = {std::move(response)};
        fails(code, [&] { provider.calendars(); });
    };
    failure({401, "fixture-token should never appear in errors"}, ExternalCalendarErrorCode::AuthenticationRequired);
    failure({403, "denied"}, ExternalCalendarErrorCode::PermissionDenied);
    failure({302, "redirect"}, ExternalCalendarErrorCode::Transport);
    failure({429, "quota"}, ExternalCalendarErrorCode::RateLimited);
    failure({403, R"({"error":{"errors":[{"reason":"userRateLimitExceeded"}]}})"}, ExternalCalendarErrorCode::RateLimited);
    failure({200, "invalid JSON"}, ExternalCalendarErrorCode::InvalidResponse);
    failure({200, R"({"items":{}})"}, ExternalCalendarErrorCode::InvalidResponse);
    failure({200, R"({"error":{"message":"unexpected"}})"}, ExternalCalendarErrorCode::InvalidResponse);
    cursor = 0;
    responses = {{200, R"({"items":[],"nextPageToken":"repeat"})"},
                 {200, R"({"items":[],"nextPageToken":"repeat"})"}};
    fails(ExternalCalendarErrorCode::InvalidResponse, [&] { provider.calendars(); });
    GoogleCalendarProvider bounded([] { return "token"; }, get, {1, 1, 1});
    cursor = 0; responses = {{200, R"({"items":[{"id":"one"},{"id":"two"}]})"}};
    fails(ExternalCalendarErrorCode::LimitExceeded, [&] { bounded.calendars(); });
    cursor = 0; responses = {{200, R"({"items":[],"nextPageToken":"next"})"}};
    fails(ExternalCalendarErrorCode::LimitExceeded, [&] { bounded.calendars(); });
    GoogleCalendarProvider expired([] { return std::string{}; }, get);
    const auto before = requests.size();
    fails(ExternalCalendarErrorCode::AuthenticationRequired, [&] { expired.calendars(); });
    CHECK(requests.size() == before);
    GoogleCalendarProvider injection([] { return "token\r\nInjected: value"; }, get);
    fails(ExternalCalendarErrorCode::AuthenticationRequired, [&] { injection.calendars(); });
    cursor = 0; responses = {{200, R"({"items":[{"id":"bad","start":{"date":"2026-10-02"},"end":{"dateTime":"2026-10-03T00:00:00Z"}}]})"}};
    fails(ExternalCalendarErrorCode::InvalidResponse, [&] { provider.events(calendars[0], window()); });
    auto empty = window(); empty.end = empty.start;
    try { provider.events(calendars[0], empty); throw std::runtime_error("accepted empty window"); }
    catch (const std::invalid_argument &) { ++checks; }
    cursor = 0; requests.clear();
    responses = {{200, R"({"items":[{"id":"inside","start":{"dateTime":"2026-10-02T00:00:00.25Z"},"end":{"dateTime":"2026-10-02T00:00:00.5Z"}},{"id":"outside","start":{"dateTime":"2026-10-02T00:00:00.75Z"},"end":{"dateTime":"2026-10-02T00:00:01Z"}}]})"}};
    const TimeRange precise{Instant::parse("2026-10-02T00:00:00.2Z"), Instant::parse("2026-10-02T00:00:00.6Z")};
    CHECK(provider.events(calendars[0], precise).size() == 1);
    CHECK(requests[0].url.find("timeMax=2026-10-02T00%3A00%3A01Z") != std::string::npos);
    cursor = 0; responses = {{200, R"({"items":[{"id":"local","start":{"dateTime":"2026-10-02T09:00:00","timeZone":"Asia/Seoul"},"end":{"dateTime":"2026-10-02T10:00:00","timeZone":"Asia/Seoul"}}]})"}};
    CHECK(provider.events(calendars[0], window())[0].range()->start == Instant::parse("2026-10-02T00:00:00Z"));
    cursor = 0; responses = {{200, std::string(16 * 1024 * 1024 + 1, 'x')}};
    fails(ExternalCalendarErrorCode::LimitExceeded, [&] { provider.calendars(); });
    CalendarObjectManager manager;
    auto managed = std::make_shared<GoogleCalendarProvider>([] { return "token"; }, get);
    cursor = 0; responses = {{200, R"({"items":[{"id":"one","summary":"Preserved"}]})"}};
    manager.login({"google-account", "google"}, managed);
    cursor = 0; responses = {{403, R"({"error":{"errors":[{"reason":"rateLimitExceeded"}]}})"}};
    fails(ExternalCalendarErrorCode::RateLimited, [&] { manager.refresh("google-account"); });
    CHECK(manager.calendars().size() == 1 && manager.accounts()[0].state == ExternalAccountState::Failed);
}
void appleTests() {
    if (AppleCalendarProvider::available()) {
    const auto status = AppleCalendarProvider::authorization();
    CHECK(status == AppleCalendarAuthorization::NotDetermined || status == AppleCalendarAuthorization::FullAccess ||
          status == AppleCalendarAuthorization::Denied || status == AppleCalendarAuthorization::Restricted ||
          status == AppleCalendarAuthorization::WriteOnly);
    AppleCalendarProvider provider;
    fails(ExternalCalendarErrorCode::LimitExceeded, [&] {
        provider.events({}, {Instant(0), Instant(367 * 86400)});
    });
    } else {
    AppleCalendarProvider provider;
    fails(ExternalCalendarErrorCode::Unsupported, [&] { provider.calendars(); });
    }
}
}
int main(int argc, char **argv) {
    try {
        const std::string suite = argc > 1 ? argv[1] : "";
        if (suite == "manager") managerTests();
        else if (suite == "google") googleTests();
        else if (suite == "apple") appleTests();
        else throw std::runtime_error("unknown suite");
        std::cout << suite << ": " << checks << " checks passed\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
