#pragma once
#include <iiCalendarIntegration/ExternalCalendar.h>

namespace iiCalendarIntegration {
// An already authenticated iisacc app session, not a display-only account JSON.
// Authentication and credential custody stay with the host's account SDK.
struct IisaccCalendarSession {
    std::string origin, subject, userId, sessionId, deviceId, email, displayName;
    Instant expiresAt;
    bool operator==(const IisaccCalendarSession &) const = default;
};
// Host-owned workspace scope. These identifiers isolate connections and device
// sources but do not assert that a person or external provider is authenticated.
struct CalendarWorkspaceIdentity {
    std::string nameSpace, ownerId, deviceId;
    bool operator==(const CalendarWorkspaceIdentity &) const = default;
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
// Provider-neutral account facade. A host opens a workspace or supplies an
// authenticated iisacc session. Context changes retire providers and caches;
// late results cannot cross the boundary. Network methods are blocking.
// Non-I/O session updates/reads can run concurrently. Join calls before destruction.
class CalendarAccountManager final {
public:
    explicit CalendarAccountManager(CalendarAccountOptions options = {});
    ~CalendarAccountManager();
    CalendarAccountManager(const CalendarAccountManager &) = delete;
    CalendarAccountManager &operator=(const CalendarAccountManager &) = delete;
    // Open or close a host-owned workspace. Reusing the same identity preserves
    // live connections; changing any component retires the prior workspace.
    void setWorkspace(std::optional<CalendarWorkspaceIdentity> identity);
    void setIisaccSession(std::optional<IisaccCalendarSession> authenticatedSession);
    // Reports whether an unexpired iisacc session is present; a generic workspace
    // is a storage boundary and does not establish user authentication.
    [[nodiscard]] bool authenticated() const;
    // Reports whether either type of connection workspace is currently open.
    [[nodiscard]] bool workspaceOpen() const;
    [[nodiscard]] std::optional<IisaccCalendarSession> iisaccSession() const;
    [[nodiscard]] std::vector<CalendarAccount> accounts() const;
    // Custom providers use an identity already authenticated by the host. An
    // initial window loads occurrences with the first list, as one snapshot.
    CalendarAccount connect(ExternalAccount account, std::shared_ptr<ExternalCalendarProvider> provider,
                            std::optional<TimeRange> initialWindow = {});
    // Built-ins resolve the external subject through authenticated profile APIs
    // and pin refreshed tokens to that subject. Host supplies OAuth/secure storage.
    CalendarAccount connectGoogle(CalendarAccessToken token, CalendarHttpGet get = {}, GoogleCalendarOptions options = {},
                                  std::optional<TimeRange> initialWindow = {});
    CalendarAccount connectMicrosoft(CalendarAccessToken token, CalendarHttpGet get = {}, MicrosoftCalendarOptions options = {},
                                     std::optional<TimeRange> initialWindow = {});
    // Device-managed EventKit source; OS authorization is an explicit host action.
    CalendarAccount connectApple(AppleCalendarOptions options = {}, std::optional<TimeRange> initialWindow = {});
    void refresh(const std::string &accountId);
    // Refresh the list and all events for this window with one atomic publish.
    // Transient failures preserve the previous committed account snapshot.
    void synchronize(const std::string &accountId, TimeRange window);
    void loadEvents(const std::string &accountId, const std::string &calendarId, TimeRange window);
    void setEnabled(const std::string &accountId, bool enabled);
    bool disconnect(const std::string &accountId);
    [[nodiscard]] std::vector<CalendarObject> calendars(const std::optional<std::string> &accountId = {}) const;
    [[nodiscard]] std::optional<CalendarObject> calendar(const std::string &id) const;
    // Metadata-only, workspace-bound connection restoration. Credentials, session
    // bindings and event caches never enter the document. Reattach providers
    // using the host's secure credentials after restore; disabled stays disabled.
    [[nodiscard]] std::string connectionDocument() const;
    void restoreConnections(const std::string &document);
    static constexpr std::size_t maximumConnectionBytes = 256 * 1024;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace iiCalendarIntegration
