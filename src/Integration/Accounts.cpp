#include <iiCalendarIntegration/Accounts.h>
#include "Http.h"
#include "../Security/Sha256.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>

namespace iiCalendarIntegration {
namespace {
using Json = nlohmann::json;
using Code = ExternalCalendarErrorCode;
[[noreturn]] void fail(Code code, const char *message) { throw ExternalCalendarError(code, message); }
// Reject nonpositive windows before changing the account state or doing I/O.
void validateWindow(const std::optional<TimeRange> &window) {
    if (window && window->end <= window->start) throw std::invalid_argument("event window must have positive duration");
}
std::string part(const std::string &value) { return std::to_string(value.size()) + ':' + value; }
std::string accountKey(const std::string &origin, const std::string &owner, const std::string &provider, const std::string &subject) {
    return "account:" + part(origin) + part(owner) + part(provider) + part(subject);
}
bool validText(const std::string &value, std::size_t maximum = 4096, bool required = true) {
    if ((required && value.empty()) || value.size() > maximum
        || std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; })) return false;
    try { (void)Json(value).dump(); return true; } catch (...) { return false; }
}
bool validToken(const std::string &token) {
    return !token.empty() && token.size() <= 16384 && token.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~+/=") == std::string::npos;
}
Code errorCode() {
    try { throw; } catch (const ExternalCalendarError &e) { return e.code(); } catch (...) { return Code::Transport; }
}
struct Profile { std::string subject, name, email; };
// Retain an identity and token fingerprint, never the bearer token itself. Each
// newly supplied credential is checked before a provider can send it to a page.
class IdentityToken {
    std::string provider_, fingerprint_;
    CalendarAccessToken source_;
    CalendarHttpGet get_;
    std::mutex mutex_;
    Profile profile_;
public:
    IdentityToken(std::string provider, CalendarAccessToken source, CalendarHttpGet get, std::string expectedSubject = {})
        : provider_(std::move(provider)), source_(std::move(source)), get_(get ? std::move(get) : detail::defaultCalendarHttpGet()) {
        if (!source_) throw std::invalid_argument("a current OAuth token callback is required");
        if (!expectedSubject.empty()) profile_.subject = std::move(expectedSubject);
    }
    std::string token() {
        std::lock_guard lock(mutex_);
        std::string token;
        try { token = source_(); } catch (...) { fail(Code::AuthenticationRequired, "calendar credential is unavailable"); }
        if (!validToken(token)) fail(Code::AuthenticationRequired, "calendar credential is invalid");
        const auto fingerprint = detail::sha256(std::as_bytes(std::span(token)));
        if (fingerprint == fingerprint_) return token;
        const auto url = provider_ == "google" ? "https://openidconnect.googleapis.com/v1/userinfo"
            : "https://graph.microsoft.com/v1.0/me?$select=id,displayName,mail,userPrincipalName";
        CalendarHttpResponse response;
        try { response = get_({url, {{"Authorization", "Bearer " + token}, {"Accept", "application/json"}}}); }
        catch (const ExternalCalendarError &e) { fail(e.code(), "external account profile request failed"); }
        catch (...) { fail(Code::Transport, "external account profile request failed"); }
        if (response.status == 401) fail(Code::AuthenticationRequired, "external account authentication is required");
        if (response.status == 403) fail(Code::PermissionDenied, "external account profile permission is required");
        if (response.status == 429) fail(Code::RateLimited, "external account profile request was rate limited");
        if (response.status != 200) fail(Code::Transport, "external account profile request failed");
        if (response.body.size() > 64 * 1024) fail(Code::LimitExceeded, "external account profile exceeds limit");
        Profile profile;
        try {
            const auto document = Json::parse(response.body, [](int depth, Json::parse_event_t, Json &) { if (depth > 16) throw std::invalid_argument("depth"); return true; });
            profile.subject = document.at(provider_ == "google" ? "sub" : "id").get<std::string>();
            profile.name = document.value(provider_ == "google" ? "name" : "displayName", std::string());
            if (provider_ == "google") profile.email = document.value("email", std::string());
            else if (document.contains("mail") && !document.at("mail").is_null()) profile.email = document.at("mail").get<std::string>();
            else profile.email = document.value("userPrincipalName", std::string());
            if (!validText(profile.subject) || !validText(profile.name, 4096, false) || !validText(profile.email, 4096, false)) throw std::invalid_argument("profile");
        } catch (...) { fail(Code::InvalidResponse, "invalid external account profile"); }
        if (!profile_.subject.empty() && profile.subject != profile_.subject)
            fail(Code::AuthenticationRequired, "calendar credential belongs to another external account");
        profile_ = std::move(profile); fingerprint_ = fingerprint;
        return token;
    }
    Profile profile() { (void)token(); std::lock_guard lock(mutex_); return profile_; }
};
}
class CalendarAccountManager::Impl {
public:
    struct Connection {
        CalendarAccount value;
        std::shared_ptr<ExternalCalendarProvider> provider;
        CalendarObjectManager objects;
        bool bound = false, busy = false;
        explicit Connection(ExternalCalendarLimits limits) : objects(limits) {}
    };
    struct Context {
        CalendarWorkspaceIdentity workspace;
        std::optional<IisaccCalendarSession> session;
        std::map<std::string, std::shared_ptr<Connection>> connections;
        explicit Context(CalendarWorkspaceIdentity owner) : workspace(std::move(owner)) {}
        explicit Context(IisaccCalendarSession owner)
            : workspace{owner.origin, owner.subject, owner.deviceId}, session(std::move(owner)) {}
        // Keep the established iisacc key encoding while separating generic
        // namespaces from authenticated origin URLs.
        std::string keyOrigin() const { return session ? session->origin : "workspace:" + workspace.nameSpace; }
        std::string key(const std::string &provider, const std::string &subject) const {
            const auto key = accountKey(keyOrigin(), workspace.ownerId, provider, subject);
            return session ? key : "workspace-" + key;
        }
    };
    CalendarAccountOptions options;
    mutable std::mutex mutex;
    mutable std::shared_ptr<Context> current;
    explicit Impl(CalendarAccountOptions value) : options(std::move(value)) {
        if (!validText(options.iisaccOrigin) || !options.now || !options.maximumAccounts || !options.limits.maximumCalendars
            || !options.limits.maximumEvents || !options.limits.maximumSnapshotEvents)
            throw std::invalid_argument("calendar account options must have positive limits and an iisacc origin/clock");
    }
    // The caller holds the account mutex. Retire the lower attachments so an
    // in-flight snapshot stops before issuing its next provider request.
    void replaceContext(std::shared_ptr<Context> replacement = {}) const {
        if (current) {
            for (const auto &entry : current->connections) entry.second->objects.retire();
        }
        current = std::move(replacement);
    }
    std::shared_ptr<Context> get(bool required = true) const { // mutex held
        if (current && current->session && current->session->expiresAt <= options.now()) replaceContext();
        if (!current && required) fail(Code::AuthenticationRequired, "an open calendar workspace is required");
        return current;
    }
    void check(const std::shared_ptr<Context> &context, const std::shared_ptr<Connection> &connection = {}) const {
        if (get(false) != context) fail(Code::StaleSession, "calendar workspace changed during operation");
        if (connection) {
            const auto found = context->connections.find(connection->value.id);
            if (found == context->connections.end() || found->second != connection || !connection->value.enabled)
                fail(Code::StaleSession, "calendar connection changed during operation");
        }
    }
    std::shared_ptr<Context> capture() const { std::lock_guard lock(mutex); return get(); }
    static CalendarAccount primary(const Context &context) {
        const auto &s = *context.session;
        return {accountKey(s.origin, s.subject, "iisacc", s.subject), s.subject, "iisacc", s.subject, s.displayName, s.email,
            true, true, CalendarAccountState::Ready, {}};
    }
    std::shared_ptr<Connection> find(const std::shared_ptr<Context> &context, const std::string &id) const {
        const auto found = context->connections.find(id);
        if (found == context->connections.end()) {
            if (context->session && id == primary(*context).id) fail(Code::PermissionDenied, "the required iisacc account is managed by its authentication SDK");
            fail(Code::NotFound, "calendar account is not connected");
        }
        return found->second;
    }
    template<class Work> void run(const std::shared_ptr<Context> &context, const std::string &id, Work work,
                                  const std::shared_ptr<Connection> &expected = {}, bool preserveSnapshotOnFailure = false) {
        std::shared_ptr<Connection> connection;
        std::shared_ptr<ExternalCalendarProvider> provider;
        bool bound = false;
        {
            std::lock_guard lock(mutex); check(context);
            if (expected && (!context->connections.contains(id) || context->connections.at(id) != expected))
                fail(Code::StaleSession, "calendar connection changed before operation");
            connection = find(context, id);
            if (!connection->value.enabled) fail(Code::PermissionDenied, "calendar account is disabled");
            if (connection->busy) fail(Code::Busy, "calendar account operation is already running");
            if (!connection->provider) fail(Code::AuthenticationRequired, "reattach calendar credentials after restoring connections");
            provider = connection->provider; bound = connection->bound; connection->busy = true;
            connection->value.state = CalendarAccountState::Loading; connection->value.error.reset();
        }
        try {
            work(connection->objects, connection->value, provider, bound);
            std::lock_guard lock(mutex); check(context, connection);
            connection->bound = true; connection->busy = false; connection->value.state = CalendarAccountState::Ready;
        } catch (...) {
            const auto code = errorCode();
            std::lock_guard lock(mutex);
            // Retirement does not make a new owner's connection observable.
            if (get(false) != context || !context->connections.contains(id) || context->connections.at(id) != connection)
                fail(Code::StaleSession, "calendar session changed during operation");
            connection->busy = false;
            if (!connection->value.enabled) fail(Code::StaleSession, "calendar account was disabled during operation");
            connection->value.error = code;
            const bool denied = code == Code::AuthenticationRequired || code == Code::PermissionDenied;
            connection->value.state = preserveSnapshotOnFailure && connection->bound ? CalendarAccountState::Ready :
                denied ? CalendarAccountState::AuthenticationRequired : CalendarAccountState::Failed;
            if (denied && !preserveSnapshotOnFailure) { connection->objects.logout(id); connection->bound = false; }
            fail(code, "calendar account operation failed");
        }
    }
    CalendarAccount connect(const std::shared_ptr<Context> &context, ExternalAccount account,
                            std::shared_ptr<ExternalCalendarProvider> provider, std::optional<TimeRange> window,
                            std::string email = {}) {
        if (!provider || !validText(account.id) || !validText(account.providerId, 128)
            || account.providerId == "iisacc" || account.providerId != provider->id()
            || !validText(account.displayName, 4096, false) || !validText(email, 4096, false))
            throw std::invalid_argument("connection requires an authenticated external identity and matching provider");
        const auto key = context->key(account.providerId, account.id);
        bool enabled = true;
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard lock(mutex); check(context);
            const auto found = context->connections.find(key);
            if (found != context->connections.end()) {
                if (found->second->busy) fail(Code::Busy, "calendar account operation is already running");
                enabled = found->second->value.enabled;
            } else if (context->connections.size() == options.maximumAccounts) fail(Code::LimitExceeded, "calendar account limit exceeded");
            connection = std::make_shared<Connection>(options.limits);
            connection->value = {key, context->workspace.ownerId, account.providerId, account.id, account.displayName, std::move(email),
                false, enabled, enabled ? CalendarAccountState::Linked : CalendarAccountState::Disabled, {}};
            connection->provider = std::move(provider); context->connections[key] = connection;
        }
        if (enabled) run(context, key, [&](CalendarObjectManager &objects, const CalendarAccount &value, const auto &source, bool) {
            objects.login({value.id, value.providerId, value.displayName}, source, window);
        }, connection);
        std::lock_guard lock(mutex); check(context);
        if (!context->connections.contains(key) || context->connections.at(key) != connection)
            fail(Code::StaleSession, "calendar connection changed during attachment");
        return connection->value;
    }
};
CalendarAccountManager::CalendarAccountManager(CalendarAccountOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
CalendarAccountManager::~CalendarAccountManager() = default;
void CalendarAccountManager::setWorkspace(std::optional<CalendarWorkspaceIdentity> identity) {
    if (identity && (!validText(identity->nameSpace) || !validText(identity->ownerId) || !validText(identity->deviceId)))
        throw std::invalid_argument("calendar workspace namespace, owner and device are required");
    std::lock_guard lock(impl_->mutex);
    if (!identity) { impl_->replaceContext(); return; }
    const auto current = impl_->get(false);
    if (current && !current->session && current->workspace == *identity) return;
    impl_->replaceContext(std::make_shared<Impl::Context>(std::move(*identity)));
}
void CalendarAccountManager::setIisaccSession(std::optional<IisaccCalendarSession> session) {
    std::lock_guard lock(impl_->mutex);
    if (!session) { impl_->replaceContext(); return; }
    if (session->origin != impl_->options.iisaccOrigin || !validText(session->subject) || !validText(session->userId, 4096, false)
        || !validText(session->sessionId) || !validText(session->deviceId) || !validText(session->email, 4096, false)
        || !validText(session->displayName, 4096, false) || session->expiresAt <= impl_->options.now()) {
        impl_->replaceContext(); fail(Code::AuthenticationRequired, "a current verified iisacc session is required");
    }
    const auto old = impl_->get(false);
    if (old && old->session && old->session->origin == session->origin && old->session->subject == session->subject
        && old->session->userId == session->userId && old->session->sessionId == session->sessionId && old->session->deviceId == session->deviceId)
    {
        old->session->email = std::move(session->email); old->session->displayName = std::move(session->displayName);
        old->session->expiresAt = session->expiresAt;
    }
    else impl_->replaceContext(std::make_shared<Impl::Context>(std::move(*session)));
}
bool CalendarAccountManager::authenticated() const {
    std::lock_guard lock(impl_->mutex); const auto context = impl_->get(false); return context && context->session.has_value();
}
bool CalendarAccountManager::workspaceOpen() const { std::lock_guard lock(impl_->mutex); return bool(impl_->get(false)); }
std::optional<IisaccCalendarSession> CalendarAccountManager::iisaccSession() const {
    std::lock_guard lock(impl_->mutex); const auto context = impl_->get(false); return context ? context->session : std::nullopt;
}
std::vector<CalendarAccount> CalendarAccountManager::accounts() const {
    std::lock_guard lock(impl_->mutex); const auto context = impl_->get(false); if (!context) return {};
    std::vector<CalendarAccount> result;
    if (context->session) result.push_back(Impl::primary(*context));
    for (const auto &[key, connection] : context->connections) { (void)key; result.push_back(connection->value); }
    return result;
}
CalendarAccount CalendarAccountManager::connect(ExternalAccount account, std::shared_ptr<ExternalCalendarProvider> provider,
                                                std::optional<TimeRange> initialWindow) {
    validateWindow(initialWindow);
    return impl_->connect(impl_->capture(), std::move(account), std::move(provider), initialWindow);
}
CalendarAccount CalendarAccountManager::connectGoogle(CalendarAccessToken token, CalendarHttpGet get, GoogleCalendarOptions options,
                                                      std::optional<TimeRange> initialWindow) {
    validateWindow(initialWindow);
    const auto context = impl_->capture();
    auto identity = std::make_shared<IdentityToken>("google", std::move(token), get);
    const auto profile = identity->profile();
    auto provider = std::make_shared<GoogleCalendarProvider>([identity] { return identity->token(); }, std::move(get), options);
    return impl_->connect(context, {profile.subject, "google", profile.name}, std::move(provider), initialWindow, profile.email);
}
CalendarAccount CalendarAccountManager::connectMicrosoft(CalendarAccessToken token, CalendarHttpGet get, MicrosoftCalendarOptions options,
                                                         std::optional<TimeRange> initialWindow) {
    validateWindow(initialWindow);
    const auto context = impl_->capture();
    auto identity = std::make_shared<IdentityToken>("microsoft", std::move(token), get);
    const auto profile = identity->profile();
    auto provider = std::make_shared<MicrosoftCalendarProvider>([identity] { return identity->token(); }, std::move(get), options);
    return impl_->connect(context, {profile.subject, "microsoft", profile.name}, std::move(provider), initialWindow, profile.email);
}
CalendarAccount CalendarAccountManager::connectGoogleForSubject(std::string expectedSubject, CalendarAccessToken token,
    CalendarHttpGet get, GoogleCalendarOptions options, std::optional<TimeRange> initialWindow) {
    validateWindow(initialWindow);
    if (!validText(expectedSubject)) throw std::invalid_argument("expected provider subject is required");
    const auto context = impl_->capture();
    auto identity = std::make_shared<IdentityToken>("google", std::move(token), get, std::move(expectedSubject));
    const auto profile = identity->profile();
    auto provider = std::make_shared<GoogleCalendarProvider>([identity] { return identity->token(); }, std::move(get), options);
    return impl_->connect(context, {profile.subject, "google", profile.name}, std::move(provider), initialWindow, profile.email);
}
CalendarAccount CalendarAccountManager::connectMicrosoftForSubject(std::string expectedSubject, CalendarAccessToken token,
    CalendarHttpGet get, MicrosoftCalendarOptions options, std::optional<TimeRange> initialWindow) {
    validateWindow(initialWindow);
    if (!validText(expectedSubject)) throw std::invalid_argument("expected provider subject is required");
    const auto context = impl_->capture();
    auto identity = std::make_shared<IdentityToken>("microsoft", std::move(token), get, std::move(expectedSubject));
    const auto profile = identity->profile();
    auto provider = std::make_shared<MicrosoftCalendarProvider>([identity] { return identity->token(); }, std::move(get), options);
    return impl_->connect(context, {profile.subject, "microsoft", profile.name}, std::move(provider), initialWindow, profile.email);
}
CalendarAccount CalendarAccountManager::connectApple(AppleCalendarOptions options, std::optional<TimeRange> initialWindow) {
    validateWindow(initialWindow);
    const auto context = impl_->capture();
    auto provider = std::make_shared<AppleCalendarProvider>(options);
    std::string name = "Device calendars";
    if (!options.sourceId.empty()) {
        const auto sources = provider->sources();
        const auto source = std::find_if(sources.begin(), sources.end(), [&](const auto &item) { return item.id == options.sourceId; });
        if (source == sources.end()) fail(Code::NotFound, "Apple calendar source was not found");
        name = source->title;
    }
    return impl_->connect(context, {part(context->workspace.deviceId) + part(options.sourceId), "apple", name}, std::move(provider), initialWindow);
}
void CalendarAccountManager::refresh(const std::string &accountId) {
    impl_->run(impl_->capture(), accountId, [](CalendarObjectManager &objects, const CalendarAccount &value, const auto &provider, bool bound) {
        if (!bound) objects.login({value.id, value.providerId, value.displayName}, provider); else objects.refresh(value.id);
    });
}
void CalendarAccountManager::synchronize(const std::string &accountId, TimeRange window) {
    validateWindow(window);
    impl_->run(impl_->capture(), accountId, [&](CalendarObjectManager &objects, const CalendarAccount &value, const auto &provider, bool bound) {
        if (!bound) objects.login({value.id, value.providerId, value.displayName}, provider, window);
        else objects.synchronize(value.id, window);
    });
}
void CalendarAccountManager::loadEvents(const std::string &accountId, const std::string &calendarId, TimeRange window) {
    impl_->run(impl_->capture(), accountId, [&](CalendarObjectManager &objects, const CalendarAccount &value, const auto &provider, bool bound) {
        if (!bound) objects.login({value.id, value.providerId, value.displayName}, provider);
        objects.loadEvents(value.id, calendarId, window);
    });
}
Event CalendarAccountManager::createEvent(const std::string &accountId, const std::string &calendarId, Event draft) {
    const auto context = impl_->capture();
    Event result;
    std::string subject;
    impl_->run(context, accountId, [&](CalendarObjectManager &objects, const CalendarAccount &value, const auto &, bool bound) {
        if (!bound) fail(Code::AuthenticationRequired, "calendar connection must be reattached before writing");
        result = objects.createEvent(value.id, calendarId, std::move(draft));
        subject = value.subject;
    }, {}, true);
    result.ownerId = context->workspace.ownerId;
    if (context->session) result.properties["iisacc.subject"] = context->session->subject;
    result.properties["external.subject"] = std::move(subject);
    return result;
}
Event CalendarAccountManager::updateOccurrence(const std::string &accountId, const std::string &calendarId,
                                               const std::string &eventId, const EventOccurrenceUpdate &change) {
    const auto context = impl_->capture();
    Event result; std::string subject;
    impl_->run(context, accountId, [&](CalendarObjectManager &objects, const CalendarAccount &value, const auto &, bool bound) {
        if (!bound) fail(Code::AuthenticationRequired, "calendar connection must be reattached before writing");
        result = objects.updateOccurrence(value.id, calendarId, eventId, change); subject = value.subject;
    }, {}, true);
    result.ownerId = context->workspace.ownerId;
    if (context->session) result.properties["iisacc.subject"] = context->session->subject;
    result.properties["external.subject"] = std::move(subject);
    return result;
}
std::vector<RemoteTaskList> CalendarAccountManager::taskLists(const std::string &accountId) {
    std::vector<RemoteTaskList> result;
    impl_->run(impl_->capture(), accountId, [&](CalendarObjectManager &objects, const CalendarAccount &value, const auto &, bool bound) {
        if (!bound) fail(Code::AuthenticationRequired, "calendar connection must be reattached before listing tasks");
        result = objects.taskLists(value.id);
    }, {}, true);
    return result;
}
std::vector<RemoteTask> CalendarAccountManager::readTasks(const std::string &accountId) {
    std::vector<RemoteTask> result;
    impl_->run(impl_->capture(), accountId, [&](CalendarObjectManager &objects, const CalendarAccount &value, const auto &, bool bound) {
        if (!bound) fail(Code::AuthenticationRequired, "calendar connection must be reattached before reading tasks");
        result = objects.readTasks(value.id);
    }, {}, true);
    return result;
}
Event CalendarAccountManager::createTask(const std::string &accountId, const std::string &calendarId,
                                         const std::string &taskListId, Event draft, bool confirmUnverifiedSource) {
    const auto context = impl_->capture();
    Event result; std::string subject;
    impl_->run(context, accountId, [&](CalendarObjectManager &objects, const CalendarAccount &value, const auto &, bool bound) {
        if (!bound) fail(Code::AuthenticationRequired, "calendar connection must be reattached before writing");
        result = objects.createTask(value.id, calendarId, taskListId, std::move(draft), confirmUnverifiedSource);
        subject = value.subject;
    }, {}, true);
    result.ownerId = context->workspace.ownerId;
    if (context->session) result.properties["iisacc.subject"] = context->session->subject;
    result.properties["external.subject"] = std::move(subject);
    return result;
}
void CalendarAccountManager::setEnabled(const std::string &accountId, bool enabled) {
    std::lock_guard lock(impl_->mutex); const auto context = impl_->get(); const auto connection = impl_->find(context, accountId);
    if (connection->value.enabled == enabled) return;
    // Replace the whole attachment so a request that has not entered the lower
    // manager yet cannot publish after a disable/re-enable cycle.
    auto replacement = std::make_shared<Impl::Connection>(impl_->options.limits);
    replacement->value = connection->value; replacement->provider = connection->provider;
    replacement->value.enabled = enabled; replacement->value.error.reset();
    replacement->value.state = !enabled ? CalendarAccountState::Disabled
        : replacement->provider ? CalendarAccountState::Linked : CalendarAccountState::AuthenticationRequired;
    connection->objects.retire();
    context->connections[accountId] = std::move(replacement);
}
bool CalendarAccountManager::disconnect(const std::string &accountId) {
    std::lock_guard lock(impl_->mutex); const auto context = impl_->get();
    if (context->session && accountId == Impl::primary(*context).id) fail(Code::PermissionDenied, "the required iisacc account cannot be disconnected here");
    const auto found = context->connections.find(accountId);
    if (found == context->connections.end()) return false;
    found->second->objects.retire(); context->connections.erase(found); return true;
}
std::vector<CalendarObject> CalendarAccountManager::calendars(const std::optional<std::string> &accountId) const {
    std::lock_guard lock(impl_->mutex); const auto context = impl_->get(); std::vector<CalendarObject> result;
    for (const auto &[key, connection] : context->connections) {
        if (!connection->value.enabled || (accountId && key != *accountId)) continue;
        auto objects = connection->objects.calendars(key);
        for (auto &object : objects) {
            for (auto &event : object.events) {
                event.ownerId = context->workspace.ownerId;
                if (context->session) event.properties["iisacc.subject"] = context->session->subject;
                event.properties["external.subject"] = connection->value.subject;
            }
            result.push_back(std::move(object));
        }
    }
    return result;
}
std::optional<CalendarObject> CalendarAccountManager::calendar(const std::string &id) const {
    auto objects = calendars(); const auto found = std::find_if(objects.begin(), objects.end(), [&](const auto &item) { return item.id == id; });
    return found == objects.end() ? std::nullopt : std::optional(std::move(*found));
}
std::string CalendarAccountManager::connectionDocument() const {
    std::lock_guard lock(impl_->mutex); const auto context = impl_->get(); Json accounts = Json::array();
    for (const auto &[id, connection] : context->connections) {
        const auto &value = connection->value;
        accounts.push_back({{"id", id}, {"provider", value.providerId}, {"subject", value.subject}, {"displayName", value.displayName},
            {"email", value.email}, {"enabled", value.enabled}});
    }
    auto root = Json{{"format", "iiCalendarIntegration.AccountConnections"}, {"schema", 1}, {"origin", context->keyOrigin()},
        {"owner", context->workspace.ownerId}, {"accounts", accounts}};
    if (!context->session) root["scope"] = "workspace";
    auto document = root.dump();
    if (document.size() > maximumConnectionBytes) fail(Code::LimitExceeded, "calendar connection document exceeds limit");
    return document;
}
void CalendarAccountManager::restoreConnections(const std::string &document) {
    const auto context = impl_->capture();
    if (document.size() > maximumConnectionBytes) fail(Code::LimitExceeded, "calendar connection document exceeds limit");
    std::map<std::string, std::shared_ptr<Impl::Connection>> connections;
    try {
        const auto root = Json::parse(document, [](int depth, Json::parse_event_t, Json &) { if (depth > 16) throw std::invalid_argument("depth"); return true; });
        const auto &format = root.at("format");
        // Preserve schema 1 documents and connection IDs through the package rename.
        if ((format != "iiCalendarIntegration.AccountConnections" && format != "iiCalendar.AccountConnections") || root.at("schema") != 1)
            throw std::invalid_argument("schema");
        if ((!context->session && root.value("scope", std::string{}) != "workspace")
            || (context->session && root.contains("scope"))
            || root.at("origin") != context->keyOrigin() || root.at("owner") != context->workspace.ownerId)
            fail(Code::PermissionDenied, "calendar connection document belongs to another workspace");
        const auto &items = root.at("accounts");
        if (!items.is_array()) throw std::invalid_argument("accounts");
        if (items.size() > impl_->options.maximumAccounts) fail(Code::LimitExceeded, "calendar account limit exceeded");
        for (const auto &item : items) {
            auto connection = std::make_shared<Impl::Connection>(impl_->options.limits); auto &value = connection->value;
            value.ownerId = context->workspace.ownerId; value.id = item.at("id").get<std::string>();
            value.providerId = item.at("provider").get<std::string>(); value.subject = item.at("subject").get<std::string>();
            value.displayName = item.at("displayName").get<std::string>(); value.email = item.at("email").get<std::string>(); value.enabled = item.at("enabled").get<bool>();
            if (!validText(value.providerId, 128) || value.providerId == "iisacc" || !validText(value.subject)
                || !validText(value.displayName, 4096, false) || !validText(value.email, 4096, false)
                || value.id != context->key(value.providerId, value.subject)) throw std::invalid_argument("identity");
            value.state = value.enabled ? CalendarAccountState::AuthenticationRequired : CalendarAccountState::Disabled;
            if (!connections.emplace(value.id, connection).second) throw std::invalid_argument("duplicate");
        }
    } catch (const ExternalCalendarError &) { throw; }
    catch (...) { fail(Code::InvalidResponse, "invalid calendar connection document"); }
    std::lock_guard lock(impl_->mutex); impl_->check(context);
    if (!context->connections.empty()) fail(Code::Busy, "restore connections before attaching providers");
    context->connections = std::move(connections);
}
} // namespace iiCalendarIntegration
