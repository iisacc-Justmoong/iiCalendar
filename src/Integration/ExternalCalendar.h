#pragma once
#include <iiCalendarIntegration/Event.h>
#include <functional>
#include <mutex>
#include <stdexcept>

namespace iiCalendarIntegration {
enum class ExternalCalendarErrorCode {
    AuthenticationRequired, PermissionDenied, RateLimited, Transport, InvalidResponse,
    NotFound, LimitExceeded, Busy, StaleSession, Unsupported, Conflict
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
    std::string sourceId{}, sourceName{};
    bool primary = false, readOnly = true;
};
// A writable task collection. isDefault identifies the account's recognized
// default; an unverified same-named device source is explicitly marked below.
struct RemoteTaskList {
    std::string id, title;
    bool isDefault = false;
    // An unverified same-named source is a candidate, not an account match.
    // The caller must obtain explicit confirmation before creating there.
    std::string sourceId{}, sourceName{};
    bool requiresConfirmation = false;
};
// A provider task is independent of any calendar. Dates and instants remain
// separate so a date-only task never acquires a fabricated local time.
struct RemoteTask {
    std::string id, remoteId, accountId, listId, listTitle, title, description;
    bool completed = false;
    std::optional<Date> dueDate;
    std::optional<Instant> start, due;
};
// The only mutable fields for one occurrence. The caller owns this value; no
// provider may infer permission to alter reminders, participants, or recurrence.
struct EventOccurrenceUpdate {
    std::string title;
    Schedule schedule;
};
// Providers return complete, bounded snapshots and expanded occurrences, not recurrence masters.
// Event.id is the provider's occurrence ID; the manager adds account/calendar namespaces.
class ExternalCalendarProvider {
  public:
    virtual ~ExternalCalendarProvider() = default;
    virtual std::string id() const = 0;
    virtual std::vector<RemoteCalendar> calendars() = 0;
    virtual std::vector<Event> events(const RemoteCalendar &calendar, TimeRange window) = 0;
    // Create one event in this provider's calendar. Existing read-only provider
    // implementations inherit an explicit Unsupported result.
    virtual Event createEvent(const RemoteCalendar &calendar, const Event &draft);
    // Update one loaded occurrence; inherited implementations reject writes.
    // The original identifies the occurrence and carries its provider version.
    virtual Event updateOccurrence(const RemoteCalendar &calendar, const Event &original, const EventOccurrenceUpdate &change);
    // Enumerate writable task destinations for this account. Read-only custom
    // providers inherit an explicit Unsupported result.
    virtual std::vector<RemoteTaskList> taskLists();
    // Enumerate collections for reading, including non-default collections.
    // Existing providers may inherit their creation-destination enumeration.
    virtual std::vector<RemoteTaskList> allTaskLists();
    // Return all tasks in one provider-owned list as an owning snapshot.
    virtual std::vector<RemoteTask> tasks(const RemoteTaskList &list);
    // Create a task in a previously resolved list; it is not an event snapshot.
    virtual Event createTask(const RemoteTaskList &list, const Event &draft, bool confirmUnverifiedSource = false);
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
    // Create in the already-loaded account/calendar without refreshing the list.
    // Call on a worker. A transport failure can leave the remote outcome unknown.
    Event createEvent(const std::string &accountId, const std::string &calendarId, Event draft);
    // Worker-only: commit the provider result to the snapshot only after success.
    Event updateOccurrence(const std::string &accountId, const std::string &calendarId,
                           const std::string &eventId, const EventOccurrenceUpdate &change);
    // Resolve this account's task lists, reusing the cached list where possible.
    [[nodiscard]] std::vector<RemoteTaskList> taskLists(const std::string &accountId);
    // Read all lists and tasks atomically on a worker; no partial result is returned.
    [[nodiscard]] std::vector<RemoteTask> readTasks(const std::string &accountId);
    // Create a task in a previously resolved list of the selected account.
    // The returned task is not added to the calendar event snapshot.
    Event createTask(const std::string &accountId, const std::string &calendarId,
                     const std::string &taskListId, Event draft, bool confirmUnverifiedSource = false);
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

// The injected transport must handle GET and JSON POST/PATCH without following
// redirects. Bodies and authorization headers must not be logged.
struct CalendarHttpRequest {
    std::string url;
    std::map<std::string, std::string> headers;
    std::string method = "GET", body{};
};
struct CalendarHttpResponse { int status = 0; std::string body; };
using CalendarHttpGet = std::function<CalendarHttpResponse(const CalendarHttpRequest &)>;
// The host owns OAuth and secure token storage. This callback supplies a current
// access token for ONE account; invoked for each request/page. Reading and
// writing require the corresponding provider-specific delegated scopes.
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
    Event createEvent(const RemoteCalendar &calendar, const Event &draft) override;
    Event updateOccurrence(const RemoteCalendar &calendar, const Event &original, const EventOccurrenceUpdate &change) override;
    std::vector<RemoteTaskList> taskLists() override;
    std::vector<RemoteTaskList> allTaskLists() override;
    std::vector<RemoteTask> tasks(const RemoteTaskList &list) override;
    Event createTask(const RemoteTaskList &list, const Event &draft, bool confirmUnverifiedSource = false) override;
  private:
    CalendarAccessToken token_;
    CalendarHttpGet get_;
    GoogleCalendarOptions options_;
    CalendarHttpResponse request(const std::string &url, const std::string &method = "GET", const std::string &body = {}, const std::string &etag = {});
};

struct MicrosoftCalendarOptions {
    std::size_t maximumPages = 100, maximumCalendars = 1000, maximumEvents = 10000;
};
// Microsoft Graph delegated calendar/task access, immutable-ID projections,
// bounded pagination, and already-expanded calendarView occurrences.
class MicrosoftCalendarProvider final : public ExternalCalendarProvider {
public:
    explicit MicrosoftCalendarProvider(CalendarAccessToken token, CalendarHttpGet get = {}, MicrosoftCalendarOptions options = {});
    std::string id() const override { return "microsoft"; }
    std::vector<RemoteCalendar> calendars() override;
    std::vector<Event> events(const RemoteCalendar &calendar, TimeRange window) override;
    Event createEvent(const RemoteCalendar &calendar, const Event &draft) override;
    Event updateOccurrence(const RemoteCalendar &calendar, const Event &original, const EventOccurrenceUpdate &change) override;
    std::vector<RemoteTaskList> taskLists() override;
    std::vector<RemoteTaskList> allTaskLists() override;
    std::vector<RemoteTask> tasks(const RemoteTaskList &list) override;
    Event createTask(const RemoteTaskList &list, const Event &draft, bool confirmUnverifiedSource = false) override;
private:
    CalendarAccessToken token_;
    CalendarHttpGet get_;
    MicrosoftCalendarOptions options_;
    CalendarHttpResponse request(const std::string &url, const std::string &method = "GET", const std::string &body = {}, const std::string &etag = {});
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
    // Reminders require a separate explicit operating-system permission.
    static AppleCalendarAuthorization remindersAuthorization();
    // Explicit host action; callback may run on an OS queue. The host supplies
    // Info.plist usage descriptions/entitlements and manages callback lifetime.
    void requestAccess(std::function<void(bool)> completion);
    void requestRemindersAccess(std::function<void(bool)> completion);
    std::vector<AppleCalendarSource> sources();
    std::vector<RemoteCalendar> calendars() override;
    std::vector<Event> events(const RemoteCalendar &calendar, TimeRange window) override;
    Event createEvent(const RemoteCalendar &calendar, const Event &draft) override;
    Event updateOccurrence(const RemoteCalendar &calendar, const Event &original, const EventOccurrenceUpdate &change) override;
    std::vector<RemoteTaskList> taskLists() override;
    Event createTask(const RemoteTaskList &list, const Event &draft, bool confirmUnverifiedSource = false) override;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace iiCalendarIntegration
