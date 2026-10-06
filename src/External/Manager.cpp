#include <iiCalendar/ExternalCalendar.h>
#include <algorithm>
#include <set>

namespace iiCalendar {
ExternalCalendarError::ExternalCalendarError(ExternalCalendarErrorCode code, const std::string &message)
    : std::runtime_error(message), code_(code) {}
namespace {
std::string component(const std::string &value) { return std::to_string(value.size()) + ":" + value; }
std::string objectId(const ExternalAccount &account, const std::string &calendar) {
    return "external:" + component(account.providerId) + component(account.id) + component(calendar);
}
void invalid(const char *message) {
    throw ExternalCalendarError(ExternalCalendarErrorCode::InvalidResponse, message);
}
}
struct CalendarObjectManager::Impl {
    struct Session {
        ExternalAccountSnapshot snapshot;
        std::shared_ptr<ExternalCalendarProvider> provider;
        std::map<std::string, CalendarObject> objects;
        bool busy = false;
    };
    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<Session>> sessions;
    ExternalCalendarLimits limits;
    CalendarRegistry registry;
    explicit Impl(ExternalCalendarLimits value) : limits(value) {}
    bool current(const std::shared_ptr<Session> &session) const {
        const auto found = sessions.find(session->snapshot.account.id);
        return found != sessions.end() && found->second == session;
    }
    template<class Fetch, class Commit>
    void run(const std::string &accountId, Fetch fetch, Commit commit) {
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(mutex);
            const auto it = sessions.find(accountId);
            if (it == sessions.end())
                throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "account is not logged in");
            session = it->second;
            if (session->busy)
                throw ExternalCalendarError(ExternalCalendarErrorCode::Busy, "account request is already running");
            session->busy = true;
            session->snapshot.state = ExternalAccountState::Loading;
            session->snapshot.error.reset();
        }
        try {
            auto result = fetch(*session);
            std::lock_guard lock(mutex);
            if (!current(session))
                throw ExternalCalendarError(ExternalCalendarErrorCode::StaleSession, "login changed during request");
            commit(*session, std::move(result));
            session->busy = false;
            session->snapshot.state = ExternalAccountState::Ready;
        } catch (...) {
            ExternalCalendarErrorCode code = ExternalCalendarErrorCode::Transport;
            try { throw; } catch (const ExternalCalendarError &error) { code = error.code(); } catch (...) {}
            std::lock_guard lock(mutex);
            if (!current(session))
                throw ExternalCalendarError(ExternalCalendarErrorCode::StaleSession, "login changed during request");
            session->busy = false;
            session->snapshot.error = code;
            const bool denied = code == ExternalCalendarErrorCode::AuthenticationRequired ||
                                code == ExternalCalendarErrorCode::PermissionDenied;
            session->snapshot.state = denied ? ExternalAccountState::AuthenticationRequired : ExternalAccountState::Failed;
            if (denied) session->objects.clear();
            throw;
        }
    }
};
CalendarObjectManager::CalendarObjectManager(ExternalCalendarLimits limits)
    : impl_(std::make_unique<Impl>(limits)) {
    if (limits.maximumCalendars == 0 || limits.maximumEvents == 0)
        throw std::invalid_argument("external calendar limits must be positive");
}
CalendarObjectManager::~CalendarObjectManager() = default;
void CalendarObjectManager::login(ExternalAccount account, std::shared_ptr<ExternalCalendarProvider> provider) {
    if (!provider || account.id.empty() || account.providerId.empty() || account.providerId != provider->id())
        throw std::invalid_argument("login requires an account and a matching calendar provider");
    auto session = std::make_shared<Impl::Session>();
    session->snapshot.account = std::move(account);
    session->provider = std::move(provider);
    const auto id = session->snapshot.account.id;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->sessions[id] = std::move(session);
    }
    refresh(id);
}
void CalendarObjectManager::refresh(const std::string &accountId) {
    impl_->run(accountId, [&](Impl::Session &session) {
        auto calendars = session.provider->calendars();
        if (calendars.size() > impl_->limits.maximumCalendars)
            throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "calendar count limit exceeded");
        std::map<std::string, CalendarObject> result;
        for (auto &remote : calendars) {
            if (remote.id.empty()) invalid("calendar has no remote id");
            try { (void)TimeZone(remote.zone); } catch (const std::exception &) { invalid("calendar has an invalid time zone"); }
            const auto id = objectId(session.snapshot.account, remote.id);
            CalendarObject object;
            object.id = id;
            object.accountId = accountId;
            object.providerId = session.snapshot.account.providerId;
            object.remote = std::move(remote);
            if (!result.emplace(id, std::move(object)).second) invalid("duplicate calendar id");
        }
        return result;
    }, [](Impl::Session &session, std::map<std::string, CalendarObject> result) {
        for (auto &[id, object] : result) {
            const auto old = session.objects.find(id);
            if (old != session.objects.end() && old->second.remote.zone == object.remote.zone &&
                old->second.remote.sourceId == object.remote.sourceId) {
                object.events = std::move(old->second.events);
                object.loadedWindow = old->second.loadedWindow;
            }
        }
        session.objects = std::move(result);
    });
}
void CalendarObjectManager::loadEvents(const std::string &accountId, const std::string &calendarId, TimeRange window) {
    if (window.end <= window.start) throw std::invalid_argument("event window must have positive duration");
    impl_->run(accountId, [&](Impl::Session &session) {
        const auto found = session.objects.find(calendarId);
        if (found == session.objects.end())
            throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "calendar does not belong to account");
        auto entries = session.provider->events(found->second.remote, window);
        if (entries.size() > impl_->limits.maximumEvents)
            throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "event count limit exceeded");
        std::set<std::string> ids;
        std::vector<Event> result;
        for (auto &event : entries) {
            if (event.id.empty() || !ids.insert(event.id).second) invalid("missing or duplicate event id");
            if (event.recurrence != RecurrenceRule{}) invalid("provider must expand recurring events");
            event.properties["external.provider"] = session.snapshot.account.providerId;
            event.properties["external.account"] = accountId;
            event.properties["external.calendar"] = found->second.remote.id;
            event.properties["external.event"] = event.id;
            event.id = calendarId + component(event.id);
            event.calendarId = calendarId;
            event.ownerId = accountId;
            event.revision = 0;
            try { event.validate(impl_->registry); }
            catch (const std::exception &) { invalid("invalid external event"); }
            const auto range = event.range();
            if (!range) invalid("external event has no schedule");
            if (event.status != EventStatus::Cancelled && range->overlaps(window)) result.push_back(std::move(event));
        }
        std::sort(result.begin(), result.end(), [](const Event &a, const Event &b) {
            return std::pair(a.range()->start, a.id) < std::pair(b.range()->start, b.id);
        });
        return result;
    }, [&](Impl::Session &session, std::vector<Event> result) {
        auto &object = session.objects.at(calendarId);
        object.events = std::move(result);
        object.loadedWindow = window;
    });
}
bool CalendarObjectManager::logout(const std::string &accountId) {
    std::lock_guard lock(impl_->mutex);
    return impl_->sessions.erase(accountId) != 0;
}
std::vector<ExternalAccountSnapshot> CalendarObjectManager::accounts() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<ExternalAccountSnapshot> result;
    for (const auto &[id, session] : impl_->sessions) { (void)id; result.push_back(session->snapshot); }
    return result;
}
std::vector<CalendarObject> CalendarObjectManager::calendars(const std::optional<std::string> &accountId) const {
    std::lock_guard lock(impl_->mutex);
    std::vector<CalendarObject> result;
    for (const auto &[id, session] : impl_->sessions) {
        if (accountId && id != *accountId) continue;
        for (const auto &[key, object] : session->objects) { (void)key; result.push_back(object); }
    }
    return result;
}
std::optional<CalendarObject> CalendarObjectManager::calendar(const std::string &id) const {
    std::lock_guard lock(impl_->mutex);
    for (const auto &[key, session] : impl_->sessions) {
        (void)key;
        const auto found = session->objects.find(id);
        if (found != session->objects.end()) return found->second;
    }
    return {};
}
} // namespace iiCalendar
