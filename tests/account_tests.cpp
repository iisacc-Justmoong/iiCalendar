#include <iiCalendar/iiCalendar.h>
#include <condition_variable>
#include <future>
#include <atomic>
#include <iostream>

using namespace iiCalendar;
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
}
void replacingAConnectionWhileItsProviderIsStarting(bool toggleEnabled) {
    class DelayedIdentity final : public Provider {
    public:
        mutable std::atomic<int> reads = 0;
        mutable std::promise<void> entered, release;
        std::string id() const override {
            if (++reads == 2) { entered.set_value(); release.get_future().wait(); }
            return "fixture";
        }
        std::vector<RemoteCalendar> calendars() override { return {{"calendar", "Retired"}}; }
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
    if (toggleEnabled) {
        CHECK(accounts.calendars(id).empty());
        accounts.refresh(id); CHECK(accounts.calendars(id).size() == 1);
    } else CHECK(accounts.calendars(id).at(0).remote.title == "Work");
}
}
int main() {
    try {
        lifecycle(); persistence(); stale(); externalIdentities(); replacingAConnectionWhileItsProviderIsStarting(false); replacingAConnectionWhileItsProviderIsStarting(true); std::cout << checks << " account checks passed\n";
    }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
