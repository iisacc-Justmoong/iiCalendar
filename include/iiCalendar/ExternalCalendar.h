#pragma once
#include "Event.h"
#include <functional>
#include <mutex>
#include <stdexcept>

namespace iiCalendar {
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
};
// Synchronous I/O: call login/refresh/loadEvents on an application worker.
// Reads return owning copies. Logout/relogin invalidate in-flight results.
class CalendarObjectManager {
  public:
    explicit CalendarObjectManager(ExternalCalendarLimits limits = {});
    ~CalendarObjectManager();
    CalendarObjectManager(const CalendarObjectManager &) = delete;
    CalendarObjectManager &operator=(const CalendarObjectManager &) = delete;
    void login(ExternalAccount account, std::shared_ptr<ExternalCalendarProvider> provider);
    void refresh(const std::string &accountId);
    void loadEvents(const std::string &accountId, const std::string &calendarId, TimeRange window);
    bool logout(const std::string &accountId);
    std::vector<ExternalAccountSnapshot> accounts() const;
    std::vector<CalendarObject> calendars(const std::optional<std::string> &accountId = {}) const;
    std::optional<CalendarObject> calendar(const std::string &id) const;
  private:
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

enum class AppleCalendarAuthorization { NotDetermined, FullAccess, Denied, Restricted, WriteOnly };
struct AppleCalendarSource { std::string id, title; };
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
} // namespace iiCalendar
