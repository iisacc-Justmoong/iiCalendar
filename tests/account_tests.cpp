#include <iiCalendarIntegration/iiCalendarIntegration.h>
#include <condition_variable>
#include <future>
#include <atomic>
#include <iostream>

using namespace iiCalendarIntegration;
namespace {
int checks = 0;
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) throw std::runtime_error(std::string(#__VA_ARGS__) + " at " + std::to_string(__LINE__)); } while (false)
template<class F> void fails(ExternalCalendarErrorCode code, F call) {
    try { call(); } catch (const ExternalCalendarError &e) { CHECK(e.code() == code); return; }
    throw std::runtime_error("expected account error");
}
IisaccCalendarSession session(std::string subject = "iisacc-user") {
    return {"https://iisacc.com", std::move(subject), "@owner", "session-1", "device-1",
        "owner@example.com", "Owner", Instant::parse("2099-01-01T00:00:00Z")};
}
TimeRange window() { return {Instant::parse("2026-10-01T00:00:00Z"), Instant::parse("2026-11-01T00:00:00Z")}; }
class Provider : public ExternalCalendarProvider {
public:
    std::string name = "fixture";
    std::optional<ExternalCalendarErrorCode> error;
    std::string id() const override { return name; }
    std::vector<RemoteCalendar> calendars() override {
        if (error) throw ExternalCalendarError(*error, "fixture");
        return {{"calendar", "Work", {}, {}, "UTC"}};
    }
    std::vector<Event> events(const RemoteCalendar &, TimeRange range) override {
        Event e; e.id = "same"; e.title = "Account event"; e.schedule = TimedSchedule{range, "UTC"}; return {e};
    }
};
// Facade dispatch must retain the attachment that owns the selected calendar.
void accountCreation() {
    class Writer final : public Provider {
    public:
        int writes = 0, reads = 0, taskWrites = 0;
        std::vector<RemoteCalendar> calendars() override {
            ++reads; auto result = Provider::calendars(); result[0].readOnly = false; return result;
        }
        std::vector<Event> events(const RemoteCalendar &calendar, TimeRange range) override {
            ++reads; return Provider::events(calendar, range);
        }
        Event createEvent(const RemoteCalendar &, const Event &draft) override {
            ++writes; if (error) throw ExternalCalendarError(*error, "fixture");
            auto saved = draft; saved.id = "created"; return saved;
        }
        std::vector<RemoteTaskList> taskLists() override { return {{"tasks", "Tasks", true}}; }
        Event createTask(const RemoteTaskList &, const Event &draft, bool) override {
            ++taskWrites; auto saved = draft; saved.id = "task"; return saved;
        }
    };
    CalendarAccountManager accounts;
    accounts.setWorkspace(CalendarWorkspaceIdentity{"test", "owner", "device"});
    auto first = std::make_shared<Writer>(), second = std::make_shared<Writer>();
    auto a = accounts.connect({"a", "fixture", "A"}, first, window());
    auto b = accounts.connect({"b", "fixture", "B"}, second, window());
    auto calendar = accounts.calendars(b.id).at(0);
    Event draft; draft.calendarId = calendar.id; draft.title = "Created"; draft.schedule = TimedSchedule{window(), "UTC"};
    auto saved = accounts.createEvent(b.id, calendar.id, draft);
    CHECK(saved.ownerId == "owner" && first->writes == 0 && second->writes == 1 && second->reads == 2);
    CHECK(accounts.calendars(b.id).at(0).events.size() == 2);
    fails(ExternalCalendarErrorCode::NotFound, [&] { (void)accounts.createEvent(a.id, calendar.id, draft); });
    CHECK(first->writes == 0 && second->writes == 1);
    auto lists = accounts.taskLists(b.id);
    draft.kind = EventKind::Task;
    auto task = accounts.createTask(b.id, calendar.id, lists.at(0).id, draft);
    CHECK(task.ownerId == "owner" && first->taskWrites == 0 && second->taskWrites == 1);
    draft.kind = EventKind::Event; second->error = ExternalCalendarErrorCode::PermissionDenied;
    fails(ExternalCalendarErrorCode::PermissionDenied, [&] { (void)accounts.createEvent(b.id, calendar.id, draft); });
    CHECK(accounts.calendars(b.id).at(0).events.size() == 2);
    CHECK(accounts.accounts().at(1).state == CalendarAccountState::Ready);
}
void lifecycle() {
    CalendarAccountManager accounts;
    auto provider = std::make_shared<Provider>();
    CHECK(!accounts.authenticated() && accounts.accounts().empty());
    fails(ExternalCalendarErrorCode::AuthenticationRequired, [&] { (void)accounts.connect({"external", "fixture", "Work"}, provider); });
    accounts.setIisaccSession(session());
    CHECK(accounts.authenticated()); CHECK(accounts.accounts().size() == 1);
    CHECK(accounts.accounts()[0].primary && accounts.accounts()[0].providerId == "iisacc");
    const auto a = accounts.connect({"same", "fixture", "Work"}, provider);
    auto second = std::make_shared<Provider>(); second->name = "other";
    const auto b = accounts.connect({"same", "other", "Personal"}, second);
    CHECK(a.id != b.id && a.ownerId == "iisacc-user");
    const auto calendar = accounts.calendars(a.id).at(0);
    accounts.loadEvents(a.id, calendar.id, window());
    CHECK(accounts.calendar(calendar.id)->events.at(0).ownerId == "iisacc-user");
    accounts.setEnabled(a.id, false);
    CHECK(accounts.calendars(a.id).empty());
    fails(ExternalCalendarErrorCode::PermissionDenied, [&] { accounts.refresh(a.id); });
    accounts.setEnabled(a.id, true); accounts.refresh(a.id);
    CHECK(accounts.calendars(a.id).size() == 1);
    provider->error = ExternalCalendarErrorCode::AuthenticationRequired;
    fails(*provider->error, [&] { accounts.refresh(a.id); });
    CHECK(accounts.calendars(a.id).empty() && accounts.calendars(b.id).size() == 1);
    fails(ExternalCalendarErrorCode::PermissionDenied, [&] { (void)accounts.disconnect(accounts.accounts()[0].id); });
    auto renewed = session(); renewed.expiresAt = renewed.expiresAt + Duration(60);
    accounts.setIisaccSession(renewed); CHECK(accounts.accounts().size() == 3);
    accounts.setIisaccSession(session("another-owner")); CHECK(accounts.accounts().size() == 1);
    CHECK(accounts.calendars().empty());
    accounts.setIisaccSession(std::nullopt); CHECK(!accounts.authenticated());
    fails(ExternalCalendarErrorCode::AuthenticationRequired, [&] { (void)accounts.calendars(); });
    auto time = Instant::parse("2026-10-16T00:00:00Z");
    CalendarAccountOptions options; options.now = [&] { return time; };
    CalendarAccountManager expiring(options); auto expires = session(); expires.expiresAt = time + Duration(1);
    expiring.setIisaccSession(expires); CHECK(expiring.authenticated()); time = time + Duration(2);
    CHECK(!expiring.authenticated() && expiring.accounts().empty());
}
void persistence() {
    CalendarAccountManager owner; owner.setIisaccSession(session());
    const auto account = owner.connect({"external", "fixture", "Work"}, std::make_shared<Provider>());
    owner.setEnabled(account.id, false);
    const auto document = owner.connectionDocument();
    CHECK(document.find("accessToken") == std::string::npos && document.find("session-1") == std::string::npos);
    auto legacyDocument = document;
    const std::string format = "iiCalendarIntegration.AccountConnections";
    CHECK(legacyDocument.find(format) != std::string::npos);
    legacyDocument.replace(legacyDocument.find(format), format.size(), "iiCalendar.AccountConnections");
    CalendarAccountManager legacy; legacy.setIisaccSession(session()); legacy.restoreConnections(legacyDocument);
    CHECK(legacy.connectionDocument() == document);
    CalendarAccountManager restored; restored.setIisaccSession(session()); restored.restoreConnections(document);
    CHECK(restored.accounts().size() == 2 && !restored.accounts()[1].enabled);
    CHECK(restored.calendars().empty());
    restored.setEnabled(account.id, true);
    fails(ExternalCalendarErrorCode::AuthenticationRequired, [&] { restored.refresh(account.id); });
    (void)restored.connect({"external", "fixture", "Work"}, std::make_shared<Provider>());
    CHECK(restored.calendars().size() == 1);
    CalendarAccountManager other; other.setIisaccSession(session("another-owner"));
    fails(ExternalCalendarErrorCode::PermissionDenied, [&] { other.restoreConnections(document); });
    CHECK(other.accounts().size() == 1);
    fails(ExternalCalendarErrorCode::InvalidResponse, [&] { other.restoreConnections("invalid"); });
    CHECK(owner.disconnect(account.id) && owner.accounts().size() == 1);
}
void stale() {
    class Blocking final : public Provider {
    public:
        std::promise<void> entered, release;
        std::vector<RemoteCalendar> calendars() override { entered.set_value(); release.get_future().wait(); return Provider::calendars(); }
    };
    CalendarAccountManager accounts; accounts.setIisaccSession(session());
    auto provider = std::make_shared<Blocking>();
    auto pending = std::async(std::launch::async, [&] {
        try { (void)accounts.connect({"external", "fixture", "Work"}, provider); }
        catch (const ExternalCalendarError &e) { return e.code(); }
        return ExternalCalendarErrorCode::InvalidResponse;
    });
    provider->entered.get_future().wait(); accounts.setIisaccSession(session("next-owner")); provider->release.set_value();
    CHECK(pending.get() == ExternalCalendarErrorCode::StaleSession);
    CHECK(accounts.accounts().size() == 1 && accounts.calendars().empty());
}
void externalIdentities() {
    CalendarAccountManager accounts; auto owner = session(); owner.userId.clear(); accounts.setIisaccSession(owner);
    std::string token = "google-token-a", googleSubject = "google-user"; int profileCalls = 0, status = 200;
    auto get = [&](const CalendarHttpRequest &request) -> CalendarHttpResponse {
        if (request.url.find("userinfo") != std::string::npos) {
            ++profileCalls;
            return {status, "{\"sub\":\"" + googleSubject + "\",\"email\":\"google@example.com\",\"name\":\"Google User\"}"};
        }
        return {200, R"({"items":[{"id":"work","summary":"Work","timeZone":"UTC"}]})"};
    };
    const auto google = accounts.connectGoogle([&] { return token; }, get);
    CHECK(google.subject == "google-user" && google.ownerId == "iisacc-user" && profileCalls == 1);
    accounts.refresh(google.id); CHECK(profileCalls == 1);
    token = "google-token-refreshed"; accounts.refresh(google.id); CHECK(profileCalls == 2);
    token = "google-token-rate-limit"; status = 429;
    fails(ExternalCalendarErrorCode::RateLimited, [&] { accounts.refresh(google.id); });
    CHECK(accounts.calendars(google.id).size() == 1);
    status = 200; googleSubject = "another-google-user"; token = "google-token-another-user";
    fails(ExternalCalendarErrorCode::AuthenticationRequired, [&] { accounts.refresh(google.id); });
    CHECK(accounts.calendars(google.id).empty());
    const auto microsoft = accounts.connectMicrosoft([] { return "microsoft-token"; }, [](const CalendarHttpRequest &request) {
        if (request.url.find("$select=id") != std::string::npos) return CalendarHttpResponse{200, R"({"id":"ms-user","displayName":"MS User","mail":null,"userPrincipalName":"work@example.com"})"};
        return CalendarHttpResponse{200, R"({"value":[{"id":"calendar","name":"Work"}]})"};
    });
    CHECK(microsoft.providerId == "microsoft" && microsoft.email == "work@example.com");
    CHECK(accounts.calendars(microsoft.id).size() == 1);
    CHECK(accounts.connectionDocument().find("microsoft-token") == std::string::npos);
    CHECK(accounts.connectionDocument().find("google-token") == std::string::npos);
    CalendarAccountManager anonymous; unsigned io = 0;
    fails(ExternalCalendarErrorCode::AuthenticationRequired, [&] {
        (void)anonymous.connectGoogle([] { return "fixture-token"; }, [&](auto &) { ++io; return CalendarHttpResponse{}; });
    });
    CHECK(io == 0);

    // Restoration pins the expected subject before any calendar endpoint is read.
    CalendarAccountManager restored;
    restored.setWorkspace(CalendarWorkspaceIdentity{"example.daymark", "owner", "device"});
    for (const bool googleProvider : {true, false}) {
        int profileReads = 0, calendarReads = 0;
        auto wrongSubject = [&](const CalendarHttpRequest &request) -> CalendarHttpResponse {
            if (request.url.find("userinfo") != std::string::npos || request.url.find("$select=id") != std::string::npos) {
                ++profileReads;
                return {200, googleProvider ? R"({"sub":"wrong"})" : R"({"id":"wrong"})"};
            }
            ++calendarReads; return {200, googleProvider ? R"({"items":[]})" : R"({"value":[]})"};
        };
        fails(ExternalCalendarErrorCode::AuthenticationRequired, [&] {
            if (googleProvider) (void)restored.connectGoogleForSubject("expected", [] { return "fixture-token"; }, wrongSubject);
            else (void)restored.connectMicrosoftForSubject("expected", [] { return "fixture-token"; }, wrongSubject);
        });
        CHECK(profileReads == 1 && calendarReads == 0);
    }
}
void genericWorkspace() {
    CalendarAccountManager accounts;
    accounts.setWorkspace(CalendarWorkspaceIdentity{"org.example.daymark", "workspace-a", "device-a"});
    CHECK(accounts.workspaceOpen() && !accounts.authenticated());
    CHECK(accounts.accounts().empty() && !accounts.iisaccSession());
    const auto connected = accounts.connect({"user", "fixture", "Work"}, std::make_shared<Provider>());
    CHECK(connected.ownerId == "workspace-a" && !connected.primary);
    const auto calendar = accounts.calendars(connected.id).at(0);
    accounts.loadEvents(connected.id, calendar.id, window());
    const auto event = accounts.calendar(calendar.id)->events.at(0);
    CHECK(event.ownerId == "workspace-a" && !event.properties.contains("iisacc.subject"));
    const auto document = accounts.connectionDocument();
    accounts.setWorkspace(CalendarWorkspaceIdentity{"org.example.daymark", "workspace-a", "device-a"});
    CHECK(accounts.calendars().size() == 1);
    CalendarAccountManager restored;
    restored.setWorkspace(CalendarWorkspaceIdentity{"org.example.daymark", "workspace-a", "device-a"});
    restored.restoreConnections(document);
    CHECK(restored.accounts().size() == 1 && restored.accounts()[0].id == connected.id);
    fails(ExternalCalendarErrorCode::AuthenticationRequired, [&] { restored.refresh(connected.id); });
    (void)restored.connect({"user", "fixture", "Work"}, std::make_shared<Provider>());
    CHECK(restored.calendars().size() == 1);
    CalendarAccountManager other;
    other.setWorkspace(CalendarWorkspaceIdentity{"org.example.daymark", "workspace-b", "device-a"});
    fails(ExternalCalendarErrorCode::PermissionDenied, [&] { other.restoreConnections(document); });
    accounts.setWorkspace(CalendarWorkspaceIdentity{"org.example.daymark", "workspace-a", "device-b"});
    CHECK(accounts.accounts().empty());
    accounts.setWorkspace(std::nullopt);
    CHECK(!accounts.workspaceOpen());
    fails(ExternalCalendarErrorCode::AuthenticationRequired, [&] { (void)accounts.calendars(); });

    accounts.setWorkspace(CalendarWorkspaceIdentity{"org.example.daymark", "workspace-a", "device-a"});
    const auto google = accounts.connectGoogle([] { return "google-token"; }, [](const CalendarHttpRequest &request) {
        if (request.url.find("userinfo") != std::string::npos)
            return CalendarHttpResponse{200, R"({"sub":"google-user","name":"Google User"})"};
        return CalendarHttpResponse{200, R"({"items":[{"id":"work","summary":"Work","timeZone":"UTC"}]})"};
    });
    const auto outlook = accounts.connectMicrosoft([] { return "microsoft-token"; }, [](const CalendarHttpRequest &request) {
        if (request.url.find("$select=id") != std::string::npos)
            return CalendarHttpResponse{200, R"({"id":"ms-user","displayName":"MS User"})"};
        return CalendarHttpResponse{200, R"({"value":[{"id":"calendar","name":"Work"}]})"};
    });
    CHECK(accounts.accounts().size() == 2 && google.ownerId == "workspace-a" && outlook.ownerId == "workspace-a");
    CHECK(accounts.calendars(google.id).size() == 1 && accounts.calendars(outlook.id).size() == 1);
    const auto before = accounts.connectionDocument();
    try { accounts.setWorkspace(CalendarWorkspaceIdentity{"", "owner", "device"}); throw std::runtime_error("invalid workspace accepted"); }
    catch (const std::invalid_argument &) {}
    CHECK(accounts.connectionDocument() == before);
    CalendarAccountOptions collisionOptions; collisionOptions.iisaccOrigin = "workspace:org.example.daymark";
    CalendarAccountManager collision(collisionOptions);
    auto collisionSession = session(); collisionSession.origin = collisionOptions.iisaccOrigin; collisionSession.subject = "workspace-a";
    collision.setIisaccSession(collisionSession);
    fails(ExternalCalendarErrorCode::PermissionDenied, [&] { collision.restoreConnections(before); });
    accounts.setIisaccSession(session());
    CHECK(accounts.accounts().size() == 1 && accounts.accounts().front().primary);
    fails(ExternalCalendarErrorCode::PermissionDenied, [&] { accounts.restoreConnections(before); });
}

// Workspace replacement retires an in-flight provider without publishing its data.
void staleWorkspace() {
    class Blocking final : public Provider {
    public:
        std::promise<void> entered, release;
        std::vector<RemoteCalendar> calendars() override { entered.set_value(); release.get_future().wait(); return Provider::calendars(); }
    };
    CalendarAccountManager accounts;
    accounts.setWorkspace(CalendarWorkspaceIdentity{"example", "first", "device"});
    auto provider = std::make_shared<Blocking>();
    auto pending = std::async(std::launch::async, [&] {
        try { (void)accounts.connect({"external", "fixture", "Work"}, provider); }
        catch (const ExternalCalendarError &error) { return error.code(); }
        return ExternalCalendarErrorCode::InvalidResponse;
    });
    provider->entered.get_future().wait();
    accounts.setWorkspace(CalendarWorkspaceIdentity{"example", "second", "device"});
    provider->release.set_value();
    CHECK(pending.get() == ExternalCalendarErrorCode::StaleSession);
    CHECK(accounts.accounts().empty() && accounts.calendars().empty());
}
// A failure on the second calendar must not publish the first calendar's new data.
void accountSnapshot() {
    class SnapshotProvider final : public Provider {
    public:
        unsigned lists = 0;
        std::string title = "Old";
        std::optional<ExternalCalendarErrorCode> secondError;
        bool thirdCalendar = false;
        std::vector<RemoteCalendar> calendars() override {
            ++lists;
            std::vector<RemoteCalendar> result{{"a", title + " A"}, {"b", title + " B"}};
            if (thirdCalendar) result.push_back({"c", title + " C"});
            return result;
        }
        std::vector<Event> events(const RemoteCalendar &calendar, TimeRange range) override {
            if (calendar.id == "b" && secondError) throw ExternalCalendarError(*secondError, "fixture");
            auto result = Provider::events(calendar, range);
            result.front().title = calendar.title;
            return result;
        }
    };

    CalendarAccountOptions options;
    options.limits.maximumSnapshotEvents = 2;
    CalendarAccountManager accounts(options);
    accounts.setWorkspace(CalendarWorkspaceIdentity{"example.daymark", "owner", "device"});
    auto provider = std::make_shared<SnapshotProvider>();
    const auto account = accounts.connect({"remote-user", "fixture", "Work"}, provider, window());
    CHECK(provider->lists == 1);
    CHECK(account.state == CalendarAccountState::Ready);
    const auto initial = accounts.calendars(account.id);
    CHECK(initial.size() == 2 && initial[0].events[0].ownerId == "owner");
    CHECK(initial[0].events[0].title == "Old A" && initial[1].events[0].title == "Old B");

    provider->title = "New";
    provider->secondError = ExternalCalendarErrorCode::RateLimited;
    fails(*provider->secondError, [&] { accounts.synchronize(account.id, window()); });
    const auto retained = accounts.calendars(account.id);
    CHECK(retained.size() == 2 && retained[0].remote.title == "Old A" && retained[1].remote.title == "Old B");
    CHECK(retained[0].events[0].title == "Old A" && retained[1].events[0].title == "Old B");
    CHECK(accounts.accounts()[0].state == CalendarAccountState::Failed);

    provider->secondError.reset();
    provider->thirdCalendar = true;
    fails(ExternalCalendarErrorCode::LimitExceeded, [&] { accounts.synchronize(account.id, window()); });
    CHECK(accounts.calendars(account.id)[0].remote.title == "Old A");
    provider->thirdCalendar = false;
    const TimeRange nextWindow{window().start + Duration(3600), window().end};
    accounts.synchronize(account.id, nextWindow);
    const auto updated = accounts.calendars(account.id);
    CHECK(updated[0].id == initial[0].id && updated[1].id == initial[1].id);
    CHECK(updated[0].loadedWindow == nextWindow && updated[1].loadedWindow == nextWindow);
    CHECK(updated[0].events[0].title == "New A" && updated[1].events[0].title == "New B");

    provider->secondError = ExternalCalendarErrorCode::PermissionDenied;
    fails(*provider->secondError, [&] { accounts.synchronize(account.id, window()); });
    CHECK(accounts.calendars(account.id).empty());
    CHECK(accounts.accounts()[0].state == CalendarAccountState::AuthenticationRequired);
}

// A disabled attachment stops issuing calendar reads, even if it is re-enabled
// under the same public account ID before the old provider call completes.
void retireSnapshot(bool retireWorkspace) {
    class BlockingProvider final : public Provider {
    public:
        std::promise<void> entered, release;
        unsigned reads = 0;
        std::vector<RemoteCalendar> calendars() override { return {{"a", "A"}, {"b", "B"}}; }
        std::vector<Event> events(const RemoteCalendar &calendar, TimeRange range) override {
            if (++reads == 1) { entered.set_value(); release.get_future().wait(); }
            return Provider::events(calendar, range);
        }
    };
    CalendarAccountManager accounts;
    accounts.setWorkspace(CalendarWorkspaceIdentity{"example.daymark", "owner", "device"});
    auto provider = std::make_shared<BlockingProvider>();
    const auto account = accounts.connect({"remote-user", "fixture", "Work"}, provider);
    auto pending = std::async(std::launch::async, [&] {
        try { accounts.synchronize(account.id, window()); }
        catch (const ExternalCalendarError &error) { return error.code(); }
        return ExternalCalendarErrorCode::InvalidResponse;
    });
    provider->entered.get_future().wait();
    if (retireWorkspace) {
        accounts.setWorkspace(std::nullopt);
        accounts.setWorkspace(CalendarWorkspaceIdentity{"example.daymark", "owner", "device"});
    } else {
        accounts.setEnabled(account.id, false);
        accounts.setEnabled(account.id, true);
    }
    provider->release.set_value();
    CHECK(pending.get() == ExternalCalendarErrorCode::StaleSession);
    CHECK(provider->reads == 1 && accounts.calendars(account.id).empty());
}

void replacingAConnectionWhileItsProviderIsStarting(bool toggleEnabled) {
    class DelayedIdentity final : public Provider {
    public:
        mutable std::atomic<int> identityReads = 0;
        unsigned calendarReads = 0;
        mutable std::promise<void> entered, release;
        std::string id() const override {
            if (++identityReads == 2) { entered.set_value(); release.get_future().wait(); }
            return "fixture";
        }
        std::vector<RemoteCalendar> calendars() override {
            ++calendarReads;
            return {{"calendar", "Retired"}};
        }
    };
    CalendarAccountManager accounts; accounts.setIisaccSession(session());
    auto retired = std::make_shared<DelayedIdentity>();
    auto pending = std::async(std::launch::async, [&] {
        try { (void)accounts.connect({"external", "fixture", "Retired"}, retired); }
        catch (const ExternalCalendarError &e) { return e.code(); }
        return ExternalCalendarErrorCode::InvalidResponse;
    });
    retired->entered.get_future().wait();
    const auto id = accounts.accounts().at(1).id;
    if (toggleEnabled) {
        accounts.setEnabled(id, false); accounts.setEnabled(id, true);
        CHECK(accounts.calendars(id).empty());
    } else {
        CHECK(accounts.disconnect(id));
        (void)accounts.connect({"external", "fixture", "Current"}, std::make_shared<Provider>());
    }
    retired->release.set_value(); CHECK(pending.get() == ExternalCalendarErrorCode::StaleSession);
    CHECK(retired->calendarReads == 0);
    if (toggleEnabled) {
        CHECK(accounts.calendars(id).empty());
        accounts.refresh(id); CHECK(accounts.calendars(id).size() == 1);
    } else CHECK(accounts.calendars(id).at(0).remote.title == "Work");
}
}
int main() {
    try {
        accountCreation(); lifecycle(); persistence(); stale(); externalIdentities(); genericWorkspace(); staleWorkspace();
        accountSnapshot(); retireSnapshot(false); retireSnapshot(true); replacingAConnectionWhileItsProviderIsStarting(false);
        replacingAConnectionWhileItsProviderIsStarting(true); std::cout << checks << " account checks passed\n";
    }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
