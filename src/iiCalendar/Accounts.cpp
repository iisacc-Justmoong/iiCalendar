#include <iiCalendar/Accounts.h>
#include <iiFileProviderNative.h>
#include "Http.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>

namespace iiCalendar {
namespace {
using Json = nlohmann::json;
using Code = ExternalCalendarErrorCode;
[[noreturn]] void fail(Code code, const char *message) { throw ExternalCalendarError(code, message); }
std::string part(const std::string &value) { return std::to_string(value.size()) + ':' + value; }
std::string accountKey(const IisaccCalendarSession &owner, const std::string &provider, const std::string &subject) {
    return "account:" + part(owner.origin) + part(owner.subject) + part(provider) + part(subject);
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
    IdentityToken(std::string provider, CalendarAccessToken source, CalendarHttpGet get)
        : provider_(std::move(provider)), source_(std::move(source)), get_(get ? std::move(get) : detail::defaultCalendarHttpGet()) {
        if (!source_) throw std::invalid_argument("a current OAuth token callback is required");
    }
    std::string token() {
        std::lock_guard lock(mutex_);
        std::string token;
        try { token = source_(); } catch (...) { fail(Code::AuthenticationRequired, "calendar credential is unavailable"); }
        if (!validToken(token)) fail(Code::AuthenticationRequired, "calendar credential is invalid");
        const auto fingerprint = iiFileProvider::native::sha256(token);
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
        IisaccCalendarSession session;
        std::map<std::string, std::shared_ptr<Connection>> connections;
        explicit Context(IisaccCalendarSession owner) : session(std::move(owner)) {}
    };
    CalendarAccountOptions options;
    mutable std::mutex mutex;
    mutable std::shared_ptr<Context> current;
    explicit Impl(CalendarAccountOptions value) : options(std::move(value)) {
        if (!validText(options.iisaccOrigin) || !options.now || !options.maximumAccounts || !options.limits.maximumCalendars || !options.limits.maximumEvents)
            throw std::invalid_argument("calendar account options must have positive limits and an iisacc origin/clock");
    }
    std::shared_ptr<Context> get(bool required = true) const { // mutex held
        if (current && current->session.expiresAt <= options.now()) current.reset();
        if (!current && required) fail(Code::AuthenticationRequired, "an authenticated iisacc session is required");
        return current;
    }
    void check(const std::shared_ptr<Context> &context, const std::shared_ptr<Connection> &connection = {}) const {
        if (get(false) != context) fail(Code::StaleSession, "iisacc calendar session changed during operation");
        if (connection) {
            const auto found = context->connections.find(connection->value.id);
            if (found == context->connections.end() || found->second != connection || !connection->value.enabled)
                fail(Code::StaleSession, "calendar connection changed during operation");
        }
    }
    std::shared_ptr<Context> capture() const { std::lock_guard lock(mutex); return get(); }
    static CalendarAccount primary(const Context &context) {
        const auto &s = context.session;
        return {accountKey(s, "iisacc", s.subject), s.subject, "iisacc", s.subject, s.displayName, s.email,
            true, true, CalendarAccountState::Ready, {}};
    }
    std::shared_ptr<Connection> find(const std::shared_ptr<Context> &context, const std::string &id) const {
        const auto found = context->connections.find(id);
        if (found == context->connections.end()) {
            if (id == primary(*context).id) fail(Code::PermissionDenied, "the required iisacc account is managed by its authentication SDK");
            fail(Code::NotFound, "calendar account is not connected");
        }
        return found->second;
    }
    template<class Work> void run(const std::shared_ptr<Context> &context, const std::string &id, Work work,
                                  const std::shared_ptr<Connection> &expected = {}) {
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
            connection->value.state = denied ? CalendarAccountState::AuthenticationRequired : CalendarAccountState::Failed;
            if (denied) { connection->objects.logout(id); connection->bound = false; }
            fail(code, "calendar account operation failed");
        }
    }
    CalendarAccount connect(const std::shared_ptr<Context> &context, ExternalAccount account,
                            std::shared_ptr<ExternalCalendarProvider> provider, std::string email = {}) {
        if (!provider || !validText(account.id) || !validText(account.providerId, 128)
            || account.providerId == "iisacc" || account.providerId != provider->id()
            || !validText(account.displayName, 4096, false) || !validText(email, 4096, false))
            throw std::invalid_argument("connection requires an authenticated external identity and matching provider");
        const auto key = accountKey(context->session, account.providerId, account.id);
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
            connection->value = {key, context->session.subject, account.providerId, account.id, account.displayName, std::move(email),
                false, enabled, enabled ? CalendarAccountState::Linked : CalendarAccountState::Disabled, {}};
            connection->provider = std::move(provider); context->connections[key] = connection;
        }
        if (enabled) run(context, key, [](CalendarObjectManager &objects, const CalendarAccount &value, const auto &source, bool) {
            objects.login({value.id, value.providerId, value.displayName}, source);
        }, connection);
        std::lock_guard lock(mutex); check(context);
        if (!context->connections.contains(key) || context->connections.at(key) != connection)
            fail(Code::StaleSession, "calendar connection changed during attachment");
        return connection->value;
    }
};
CalendarAccountManager::CalendarAccountManager(CalendarAccountOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
CalendarAccountManager::~CalendarAccountManager() = default;
void CalendarAccountManager::setIisaccSession(std::optional<IisaccCalendarSession> session) {
    std::lock_guard lock(impl_->mutex);
    if (!session) { impl_->current.reset(); return; }
    if (session->origin != impl_->options.iisaccOrigin || !validText(session->subject) || !validText(session->userId, 4096, false)
        || !validText(session->sessionId) || !validText(session->deviceId) || !validText(session->email, 4096, false)
        || !validText(session->displayName, 4096, false) || session->expiresAt <= impl_->options.now()) {
        impl_->current.reset(); fail(Code::AuthenticationRequired, "a current verified iisacc session is required");
    }
    const auto old = impl_->get(false);
    if (old && old->session.origin == session->origin && old->session.subject == session->subject
        && old->session.userId == session->userId && old->session.sessionId == session->sessionId && old->session.deviceId == session->deviceId)
    {
        old->session.email = std::move(session->email); old->session.displayName = std::move(session->displayName);
        old->session.expiresAt = session->expiresAt;
    }
    else impl_->current = std::make_shared<Impl::Context>(std::move(*session));
}
bool CalendarAccountManager::authenticated() const { std::lock_guard lock(impl_->mutex); return bool(impl_->get(false)); }
std::optional<IisaccCalendarSession> CalendarAccountManager::iisaccSession() const {
    std::lock_guard lock(impl_->mutex); const auto context = impl_->get(false); return context ? std::optional(context->session) : std::nullopt;
}
std::vector<CalendarAccount> CalendarAccountManager::accounts() const {
    std::lock_guard lock(impl_->mutex); const auto context = impl_->get(false); if (!context) return {};
    std::vector<CalendarAccount> result{Impl::primary(*context)};
    for (const auto &[key, connection] : context->connections) { (void)key; result.push_back(connection->value); }
    return result;
}
CalendarAccount CalendarAccountManager::connect(ExternalAccount account, std::shared_ptr<ExternalCalendarProvider> provider) {
    return impl_->connect(impl_->capture(), std::move(account), std::move(provider));
}
CalendarAccount CalendarAccountManager::connectGoogle(CalendarAccessToken token, CalendarHttpGet get, GoogleCalendarOptions options) {
    const auto context = impl_->capture();
    auto identity = std::make_shared<IdentityToken>("google", std::move(token), get);
    const auto profile = identity->profile();
    auto provider = std::make_shared<GoogleCalendarProvider>([identity] { return identity->token(); }, std::move(get), options);
    return impl_->connect(context, {profile.subject, "google", profile.name}, std::move(provider), profile.email);
}
CalendarAccount CalendarAccountManager::connectMicrosoft(CalendarAccessToken token, CalendarHttpGet get, MicrosoftCalendarOptions options) {
    const auto context = impl_->capture();
    auto identity = std::make_shared<IdentityToken>("microsoft", std::move(token), get);
    const auto profile = identity->profile();
    auto provider = std::make_shared<MicrosoftCalendarProvider>([identity] { return identity->token(); }, std::move(get), options);
    return impl_->connect(context, {profile.subject, "microsoft", profile.name}, std::move(provider), profile.email);
}
CalendarAccount CalendarAccountManager::connectApple(AppleCalendarOptions options) {
    const auto context = impl_->capture();
    auto provider = std::make_shared<AppleCalendarProvider>(options);
    std::string name = "Device calendars";
    if (!options.sourceId.empty()) {
        const auto sources = provider->sources();
        const auto source = std::find_if(sources.begin(), sources.end(), [&](const auto &item) { return item.id == options.sourceId; });
        if (source == sources.end()) fail(Code::NotFound, "Apple calendar source was not found");
        name = source->title;
    }
    return impl_->connect(context, {part(context->session.deviceId) + part(options.sourceId), "apple", name}, std::move(provider));
}
void CalendarAccountManager::refresh(const std::string &accountId) {
    impl_->run(impl_->capture(), accountId, [](CalendarObjectManager &objects, const CalendarAccount &value, const auto &provider, bool bound) {
        if (!bound) objects.login({value.id, value.providerId, value.displayName}, provider); else objects.refresh(value.id);
    });
}
void CalendarAccountManager::loadEvents(const std::string &accountId, const std::string &calendarId, TimeRange window) {
    impl_->run(impl_->capture(), accountId, [&](CalendarObjectManager &objects, const CalendarAccount &value, const auto &provider, bool bound) {
        if (!bound) objects.login({value.id, value.providerId, value.displayName}, provider);
        objects.loadEvents(value.id, calendarId, window);
    });
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
    context->connections[accountId] = std::move(replacement);
}
bool CalendarAccountManager::disconnect(const std::string &accountId) {
    std::lock_guard lock(impl_->mutex); const auto context = impl_->get();
    if (accountId == Impl::primary(*context).id) fail(Code::PermissionDenied, "the required iisacc account cannot be disconnected here");
    const auto found = context->connections.find(accountId);
    if (found == context->connections.end()) return false;
    found->second->objects.logout(accountId); context->connections.erase(found); return true;
}
std::vector<CalendarObject> CalendarAccountManager::calendars(const std::optional<std::string> &accountId) const {
    std::lock_guard lock(impl_->mutex); const auto context = impl_->get(); std::vector<CalendarObject> result;
    for (const auto &[key, connection] : context->connections) {
        if (!connection->value.enabled || (accountId && key != *accountId)) continue;
        auto objects = connection->objects.calendars(key);
        for (auto &object : objects) {
            for (auto &event : object.events) {
                event.ownerId = context->session.subject; event.properties["iisacc.subject"] = context->session.subject;
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
    auto document = Json{{"format", "iiCalendar.AccountConnections"}, {"schema", 1}, {"origin", context->session.origin},
        {"owner", context->session.subject}, {"accounts", accounts}}.dump();
    if (document.size() > maximumConnectionBytes) fail(Code::LimitExceeded, "calendar connection document exceeds limit");
    return document;
}
void CalendarAccountManager::restoreConnections(const std::string &document) {
    const auto context = impl_->capture();
    if (document.size() > maximumConnectionBytes) fail(Code::LimitExceeded, "calendar connection document exceeds limit");
    std::map<std::string, std::shared_ptr<Impl::Connection>> connections;
    try {
        const auto root = Json::parse(document, [](int depth, Json::parse_event_t, Json &) { if (depth > 16) throw std::invalid_argument("depth"); return true; });
        if (root.at("format") != "iiCalendar.AccountConnections" || root.at("schema") != 1) throw std::invalid_argument("schema");
        if (root.at("origin") != context->session.origin || root.at("owner") != context->session.subject)
            fail(Code::PermissionDenied, "calendar connection document belongs to another iisacc account");
        const auto &items = root.at("accounts");
        if (!items.is_array()) throw std::invalid_argument("accounts");
        if (items.size() > impl_->options.maximumAccounts) fail(Code::LimitExceeded, "calendar account limit exceeded");
        for (const auto &item : items) {
            auto connection = std::make_shared<Impl::Connection>(impl_->options.limits); auto &value = connection->value;
            value.ownerId = context->session.subject; value.id = item.at("id").get<std::string>();
            value.providerId = item.at("provider").get<std::string>(); value.subject = item.at("subject").get<std::string>();
            value.displayName = item.at("displayName").get<std::string>(); value.email = item.at("email").get<std::string>(); value.enabled = item.at("enabled").get<bool>();
            if (!validText(value.providerId, 128) || value.providerId == "iisacc" || !validText(value.subject)
                || !validText(value.displayName, 4096, false) || !validText(value.email, 4096, false)
                || value.id != accountKey(context->session, value.providerId, value.subject)) throw std::invalid_argument("identity");
            value.state = value.enabled ? CalendarAccountState::AuthenticationRequired : CalendarAccountState::Disabled;
            if (!connections.emplace(value.id, connection).second) throw std::invalid_argument("duplicate");
        }
    } catch (const ExternalCalendarError &) { throw; }
    catch (...) { fail(Code::InvalidResponse, "invalid calendar connection document"); }
    std::lock_guard lock(impl_->mutex); impl_->check(context);
    if (!context->connections.empty()) fail(Code::Busy, "restore connections before attaching providers");
    context->connections = std::move(connections);
}
void CalendarAccountManager::saveConnections(const std::filesystem::path &file) const {
    iiFileProvider::native::File::write(file, connectionDocument());
}
void CalendarAccountManager::loadConnections(const std::filesystem::path &file) {
    (void)impl_->capture(); restoreConnections(iiFileProvider::native::File::read(file, maximumConnectionBytes));
}
} // namespace iiCalendar
