#pragma once
#include <iiCalendarIntegration/Event.h>
#include <functional>
#include <mutex>
#include <stdexcept>

namespace iiCalendarIntegration {
enum class ExternalCalendarErrorCode {
    AuthenticationRequired, PermissionDenied, RateLimited, Transport, InvalidResponse,
    NotFound, LimitExceeded, Busy, StaleSession, Unsupported
};
class ExternalCalendarError : public std::runtime_error {
  public:
    ExternalCalendarError(ExternalCalendarErrorCode code, const std::string &message);
    ExternalCalendarErrorCode code() const noexcept { return code_; }
  private:
    ExternalCalendarErrorCode code_;
};
struct ExternalAccount {
    std::string id, providerId, displayName; // Stable login identity, never an access token.
    bool operator==(const ExternalAccount &) const = default;
};
struct RemoteCalendar {
    std::string id, title, description, color;
    std::string zone = "UTC";
    std::string sourceId, sourceName;
    bool primary = false, readOnly = true;
};
// Providers return complete, bounded snapshots and expanded occurrences, not recurrence masters.
// Event.id is the provider's occurrence ID; the manager adds account/calendar namespaces.
class ExternalCalendarProvider {
  public:
    virtual ~ExternalCalendarProvider() = default;
    virtual std::string id() const = 0;
    virtual std::vector<RemoteCalendar> calendars() = 0;
    virtual std::vector<Event> events(const RemoteCalendar &calendar, TimeRange window) = 0;
};
struct CalendarObject {
    std::string id, accountId, providerId;
    RemoteCalendar remote;
    std::vector<Event> events;
    std::optional<TimeRange> loadedWindow;
};
enum class ExternalAccountState { Loading, Ready, Failed, AuthenticationRequired };
struct ExternalAccountSnapshot {
    ExternalAccount account;
    ExternalAccountState state = ExternalAccountState::Loading;
    std::optional<ExternalCalendarErrorCode> error;
};
struct ExternalCalendarLimits {
    std::size_t maximumCalendars = 1000, maximumEvents = 10000;
    // Total retained events in one account-wide snapshot.
    std::size_t maximumSnapshotEvents = 100000;
};
class CalendarAccountManager;
// Run synchronous provider operations on an application worker.
// Reads return owning copies. Logout/relogin invalidate in-flight results.
class CalendarObjectManager {
  public:
    explicit CalendarObjectManager(ExternalCalendarLimits limits = {});
    ~CalendarObjectManager();
    CalendarObjectManager(const CalendarObjectManager &) = delete;
    CalendarObjectManager &operator=(const CalendarObjectManager &) = delete;
    // Supply a window to publish the initial calendar list and events together.
    void login(ExternalAccount account, std::shared_ptr<ExternalCalendarProvider> provider,
               std::optional<TimeRange> window = {});
    void refresh(const std::string &accountId);
    // Replace the whole account snapshot once the list and every requested calendar succeed.
    void synchronize(const std::string &accountId, TimeRange window);
    void loadEvents(const std::string &accountId, const std::string &calendarId, TimeRange window);
    bool logout(const std::string &accountId);
    std::vector<ExternalAccountSnapshot> accounts() const;
    std::vector<CalendarObject> calendars(const std::optional<std::string> &accountId = {}) const;
    std::optional<CalendarObject> calendar(const std::string &id) const;
  private:
    friend class CalendarAccountManager;
    // The account facade permanently closes an attachment when it replaces it.
    void retire();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct CalendarHttpRequest {
    std::string url;
    std::map<std::string, std::string> headers;
};
struct CalendarHttpResponse { int status = 0; std::string body; };
using CalendarHttpGet = std::function<CalendarHttpResponse(const CalendarHttpRequest &)>;
// The host owns OAuth and secure token storage. This callback supplies a current
// calendar.readonly access token for ONE account; invoked for each request/page.
using CalendarAccessToken = std::function<std::string()>;
struct GoogleCalendarOptions {
    std::size_t maximumPages = 100, maximumCalendars = 1000, maximumEvents = 10000;
};
class GoogleCalendarProvider final : public ExternalCalendarProvider {
  public:
    explicit GoogleCalendarProvider(CalendarAccessToken token, CalendarHttpGet get = {},
                                    GoogleCalendarOptions options = {});
    std::string id() const override { return "google"; }
    std::vector<RemoteCalendar> calendars() override;
    std::vector<Event> events(const RemoteCalendar &calendar, TimeRange window) override;
  private:
    CalendarAccessToken token_;
    CalendarHttpGet get_;
    GoogleCalendarOptions options_;
    CalendarHttpResponse request(const std::string &url);
};

struct MicrosoftCalendarOptions {
    std::size_t maximumPages = 100, maximumCalendars = 1000, maximumEvents = 10000;
};
// Delegated Microsoft Graph Calendars.Read. UTC/immutable-ID projections,
// bounded pagination, and already-expanded calendarView occurrences.
class MicrosoftCalendarProvider final : public ExternalCalendarProvider {
public:
    explicit MicrosoftCalendarProvider(CalendarAccessToken token, CalendarHttpGet get = {}, MicrosoftCalendarOptions options = {});
    std::string id() const override { return "microsoft"; }
    std::vector<RemoteCalendar> calendars() override;
    std::vector<Event> events(const RemoteCalendar &calendar, TimeRange window) override;
private:
    CalendarAccessToken token_;
    CalendarHttpGet get_;
    MicrosoftCalendarOptions options_;
    CalendarHttpResponse request(const std::string &url);
};

enum class AppleCalendarAuthorization { NotDetermined, FullAccess, Denied, Restricted, WriteOnly };
struct AppleCalendarSource {
    std::string id, title;
    // EventKit uses this type for both iCloud and other CalDAV sources.
    bool calDav = false;
};
struct AppleCalendarOptions {
    std::string sourceId; // Empty means all device sources, including iCloud/Google/Exchange.
    std::size_t maximumCalendars = 1000, maximumEvents = 10000;
};
class AppleCalendarProvider final : public ExternalCalendarProvider {
  public:
    explicit AppleCalendarProvider(AppleCalendarOptions options = {});
    ~AppleCalendarProvider() override;
    std::string id() const override { return "apple"; }
    static bool available() noexcept;
    static AppleCalendarAuthorization authorization();
    // Explicit host action; callback may run on an OS queue. The host supplies
    // Info.plist usage descriptions/entitlements and manages callback lifetime.
    void requestAccess(std::function<void(bool)> completion);
    std::vector<AppleCalendarSource> sources();
    std::vector<RemoteCalendar> calendars() override;
    std::vector<Event> events(const RemoteCalendar &calendar, TimeRange window) override;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace iiCalendarIntegration
