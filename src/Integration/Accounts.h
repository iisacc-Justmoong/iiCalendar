#pragma once
#include <iiCalendar/ExternalCalendar.h>

namespace iiCalendar {
// An already authenticated iisacc app session, not a display-only account JSON.
// Authentication and credential custody stay with the host's account SDK.
struct IisaccCalendarSession {
    std::string origin, subject, userId, sessionId, deviceId, email, displayName;
    Instant expiresAt;
    bool operator==(const IisaccCalendarSession &) const = default;
};
enum class CalendarAccountState { Linked, Loading, Ready, Disabled, Failed, AuthenticationRequired };
struct CalendarAccount {
    std::string id, ownerId, providerId, subject, displayName, email;
    bool primary = false, enabled = true;
    CalendarAccountState state = CalendarAccountState::Linked;
    std::optional<ExternalCalendarErrorCode> error;
};
struct CalendarAccountOptions {
    std::string iisaccOrigin = "https://iisacc.com";
    std::size_t maximumAccounts = 32; // External connections, excluding the primary.
    ExternalCalendarLimits limits;
    std::function<Instant()> now = [] { return Clock::sample().wallTime; };
};
// Native account facade. Every external operation requires an unexpired iisacc
// session. Session changes retire providers/caches; late results cannot cross
// owner/session boundaries. Network methods are blocking; use an app I/O worker.
// Non-I/O session updates/reads can run concurrently. Join calls before destruction.
class CalendarAccountManager final {
public:
    explicit CalendarAccountManager(CalendarAccountOptions options = {});
    ~CalendarAccountManager();
    CalendarAccountManager(const CalendarAccountManager &) = delete;
    CalendarAccountManager &operator=(const CalendarAccountManager &) = delete;
    void setIisaccSession(std::optional<IisaccCalendarSession> authenticatedSession);
    [[nodiscard]] bool authenticated() const;
    [[nodiscard]] std::optional<IisaccCalendarSession> iisaccSession() const;
    [[nodiscard]] std::vector<CalendarAccount> accounts() const;
    // Custom providers use an identity already authenticated by the host.
    CalendarAccount connect(ExternalAccount account, std::shared_ptr<ExternalCalendarProvider> provider);
    // Built-ins resolve the external subject through authenticated profile APIs
    // and pin refreshed tokens to that subject. Host supplies OAuth/secure storage.
    CalendarAccount connectGoogle(CalendarAccessToken token, CalendarHttpGet get = {}, GoogleCalendarOptions options = {});
    CalendarAccount connectMicrosoft(CalendarAccessToken token, CalendarHttpGet get = {}, MicrosoftCalendarOptions options = {});
    // Device-managed EventKit source; OS authorization is an explicit host action.
    CalendarAccount connectApple(AppleCalendarOptions options = {});
    void refresh(const std::string &accountId);
    void loadEvents(const std::string &accountId, const std::string &calendarId, TimeRange window);
    void setEnabled(const std::string &accountId, bool enabled);
    bool disconnect(const std::string &accountId);
    [[nodiscard]] std::vector<CalendarObject> calendars(const std::optional<std::string> &accountId = {}) const;
    [[nodiscard]] std::optional<CalendarObject> calendar(const std::string &id) const;
    // Metadata-only, owner-bound connection restoration. Credentials, session
    // bindings and event caches never enter the document. Reattach providers
    // using the host's secure credentials after restore; disabled stays disabled.
    [[nodiscard]] std::string connectionDocument() const;
    void restoreConnections(const std::string &document);
    static constexpr std::size_t maximumConnectionBytes = 256 * 1024;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace iiCalendar
