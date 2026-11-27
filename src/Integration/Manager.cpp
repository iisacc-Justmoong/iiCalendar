#include <iiCalendarIntegration/ExternalCalendar.h>
#include <algorithm>
#include <set>

namespace iiCalendarIntegration {
ExternalCalendarError::ExternalCalendarError(ExternalCalendarErrorCode code, const std::string &message)
    : std::runtime_error(message), code_(code) {}
Event ExternalCalendarProvider::createEvent(const RemoteCalendar &, const Event &) {
    throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "calendar provider does not support event creation");
}
Event ExternalCalendarProvider::updateOccurrence(const RemoteCalendar &, const Event &, const EventOccurrenceUpdate &) {
    throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "calendar provider does not support occurrence editing");
}
std::vector<RemoteTaskList> ExternalCalendarProvider::taskLists() {
    throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "calendar provider does not support tasks");
}
std::vector<RemoteTaskList> ExternalCalendarProvider::allTaskLists() { return taskLists(); }
std::vector<RemoteTask> ExternalCalendarProvider::tasks(const RemoteTaskList &) {
    throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "calendar provider does not support reading tasks");
}
Event ExternalCalendarProvider::createTask(const RemoteTaskList &, const Event &, bool) {
    throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "calendar provider does not support task creation");
}
namespace {
std::string component(const std::string &value) { return std::to_string(value.size()) + ":" + value; }
std::string objectId(const ExternalAccount &account, const std::string &calendar) {
    return "external:" + component(account.providerId) + component(account.id) + component(calendar);
}
void requireWindow(TimeRange window) {
    if (window.end <= window.start) throw std::invalid_argument("event window must have positive duration");
}
[[noreturn]] void invalid(const char *message) {
    throw ExternalCalendarError(ExternalCalendarErrorCode::InvalidResponse, message);
}
}
struct CalendarObjectManager::Impl {
    struct Session {
        ExternalAccountSnapshot snapshot;
        std::shared_ptr<ExternalCalendarProvider> provider;
        std::map<std::string, CalendarObject> objects;
        std::vector<RemoteTaskList> taskLists;
        bool taskListsLoaded = false;
        bool busy = false;
    };
    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<Session>> sessions;
    bool retired = false;
    ExternalCalendarLimits limits;
    CalendarRegistry registry;
    explicit Impl(ExternalCalendarLimits value) : limits(value) {}
    bool current(const Session &session) const {
        const auto found = sessions.find(session.snapshot.account.id);
        return found != sessions.end() && found->second.get() == &session;
    }
    // Stop scheduling calendar reads once this account attachment is retired.
    void requireCurrent(const Session &session) const {
        std::lock_guard lock(mutex);
        if (!current(session))
            throw ExternalCalendarError(ExternalCalendarErrorCode::StaleSession, "login changed during request");
    }
    template<class Fetch, class Commit>
    void run(const std::string &accountId, Fetch fetch, Commit commit,
             const std::shared_ptr<Session> &expected = {}, bool preserveSnapshotOnFailure = false) {
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(mutex);
            const auto it = sessions.find(accountId);
            if (retired || (expected && (it == sessions.end() || it->second != expected)))
                throw ExternalCalendarError(ExternalCalendarErrorCode::StaleSession, "login changed before request");
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
            if (!current(*session))
                throw ExternalCalendarError(ExternalCalendarErrorCode::StaleSession, "login changed during request");
            commit(*session, std::move(result));
            session->busy = false;
            session->snapshot.state = ExternalAccountState::Ready;
        } catch (...) {
            ExternalCalendarErrorCode code = ExternalCalendarErrorCode::Transport;
            try { throw; } catch (const ExternalCalendarError &error) { code = error.code(); } catch (...) {}
            std::lock_guard lock(mutex);
            if (!current(*session))
                throw ExternalCalendarError(ExternalCalendarErrorCode::StaleSession, "login changed during request");
            session->busy = false;
            session->snapshot.error = code;
            const bool denied = code == ExternalCalendarErrorCode::AuthenticationRequired ||
                                code == ExternalCalendarErrorCode::PermissionDenied;
            session->snapshot.state = preserveSnapshotOnFailure ? ExternalAccountState::Ready :
                denied ? ExternalAccountState::AuthenticationRequired : ExternalAccountState::Failed;
            if (denied && !preserveSnapshotOnFailure) session->objects.clear();
            throw;
        }
    }
    // Validate provider metadata and construct the account-scoped calendar IDs.
    std::map<std::string, CalendarObject> collectCalendars(const Session &session) const {
        auto calendars = session.provider->calendars();
        if (calendars.size() > limits.maximumCalendars)
            throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "calendar count limit exceeded");
        std::map<std::string, CalendarObject> result;
        for (auto &remote : calendars) {
            if (remote.id.empty()) invalid("calendar has no remote id");
            try { (void)TimeZone(remote.zone); } catch (const std::exception &) { invalid("calendar has an invalid time zone"); }
            const auto &account = session.snapshot.account;
            const auto id = objectId(account, remote.id);
            CalendarObject object;
            object.id = id;
            object.accountId = account.id;
            object.providerId = account.providerId;
            object.remote = std::move(remote);
            if (!result.emplace(id, std::move(object)).second) invalid("duplicate calendar id");
        }
        return result;
    }

    // Normalize provider identifiers once for both reads and created occurrences.
    Event projectEvent(Event event, const CalendarObject &calendar) const {
        if (event.id.empty()) invalid("event has no remote id");
        if (event.recurrence != RecurrenceRule{}) invalid("provider must expand recurring events");
        event.properties["external.provider"] = calendar.providerId;
        event.properties["external.account"] = calendar.accountId;
        event.properties["external.calendar"] = calendar.remote.id;
        event.properties["external.event"] = event.id;
        event.id = calendar.id + component(event.id);
        event.calendarId = calendar.id;
        event.ownerId = calendar.accountId;
        event.revision = 0;
        try { event.validate(registry); }
        catch (const std::exception &) { invalid("invalid external event"); }
        if (!event.range()) invalid("external event has no schedule");
        return event;
    }

    // Both per-calendar reads and account snapshots share the occurrence checks.
    std::vector<Event> collectEvents(const Session &session, const CalendarObject &calendar,
                                     TimeRange window) const {
        auto entries = session.provider->events(calendar.remote, window);
        if (entries.size() > limits.maximumEvents)
            throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "event count limit exceeded");
        std::set<std::string> ids;
        std::vector<Event> result;
        for (auto &event : entries) {
            if (event.id.empty() || !ids.insert(event.id).second) invalid("missing or duplicate event id");
            auto projected = projectEvent(std::move(event), calendar);
            if (projected.status != EventStatus::Cancelled && projected.range()->overlaps(window)) result.push_back(std::move(projected));
        }
        std::sort(result.begin(), result.end(), [](const Event &a, const Event &b) {
            return std::pair(a.range()->start, a.id) < std::pair(b.range()->start, b.id);
        });
        return result;
    }

    // Stage provider work outside the mutex. A query window requests a complete
    // snapshot, while a list-only refresh preserves events for compatible IDs.
    void refresh(const std::string &accountId, std::optional<TimeRange> window = {},
                 const std::shared_ptr<Session> &expected = {}) {
        run(accountId, [&](const Session &session) {
            auto result = collectCalendars(session);
            if (window) {
                auto remaining = limits.maximumSnapshotEvents;
                for (auto &[id, calendar] : result) {
                    (void)id;
                    requireCurrent(session);
                    auto events = collectEvents(session, calendar, *window);
                    if (events.size() > remaining)
                        throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "account event snapshot exceeds limit");
                    remaining -= events.size();
                    calendar.events = std::move(events);
                    calendar.loadedWindow = *window;
                }
            }
            return result;
        }, [&](Session &session, std::map<std::string, CalendarObject> result) {
            if (!window) {
                for (auto &[id, object] : result) {
                    const auto old = session.objects.find(id);
                    if (old != session.objects.end() && old->second.remote.zone == object.remote.zone &&
                        old->second.remote.sourceId == object.remote.sourceId) {
                        object.events = std::move(old->second.events);
                        object.loadedWindow = old->second.loadedWindow;
                    }
                }
            }
            session.objects = std::move(result);
        }, expected);
    }
};
CalendarObjectManager::CalendarObjectManager(ExternalCalendarLimits limits)
    : impl_(std::make_unique<Impl>(limits)) {
    if (limits.maximumCalendars == 0 || limits.maximumEvents == 0 || limits.maximumSnapshotEvents == 0)
        throw std::invalid_argument("external calendar limits must be positive");
}
CalendarObjectManager::~CalendarObjectManager() = default;
void CalendarObjectManager::login(ExternalAccount account, std::shared_ptr<ExternalCalendarProvider> provider,
                                  std::optional<TimeRange> window) {
    if (window) requireWindow(*window);
    if (!provider || account.id.empty() || account.providerId.empty() || account.providerId != provider->id())
        throw std::invalid_argument("login requires an account and a matching calendar provider");
    auto session = std::make_shared<Impl::Session>();
    session->snapshot.account = std::move(account);
    session->provider = std::move(provider);
    const auto id = session->snapshot.account.id;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->retired)
            throw ExternalCalendarError(ExternalCalendarErrorCode::StaleSession, "calendar attachment was retired before login");
        impl_->sessions[id] = session;
    }
    impl_->refresh(id, window, session);
}
void CalendarObjectManager::refresh(const std::string &accountId) {
    impl_->refresh(accountId);
}
void CalendarObjectManager::synchronize(const std::string &accountId, TimeRange window) {
    requireWindow(window);
    impl_->refresh(accountId, window);
}
void CalendarObjectManager::loadEvents(const std::string &accountId, const std::string &calendarId, TimeRange window) {
    requireWindow(window);
    impl_->run(accountId, [&](const Impl::Session &session) {
        const auto found = session.objects.find(calendarId);
        if (found == session.objects.end())
            throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "calendar does not belong to account");
        return impl_->collectEvents(session, found->second, window);
    }, [&](Impl::Session &session, std::vector<Event> result) {
        auto &object = session.objects.at(calendarId);
        object.events = std::move(result);
        object.loadedWindow = window;
    });
}
Event CalendarObjectManager::createEvent(const std::string &accountId, const std::string &calendarId, Event draft) {
    if (draft.kind != EventKind::Event || draft.calendarId != calendarId)
        throw std::invalid_argument("a non-recurring event for the selected calendar is required");
    if (draft.recurrence != RecurrenceRule{})
        throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "recurring event creation is not supported yet");
    if (draft.id.empty()) draft.id = "draft";
    draft.validate(impl_->registry);
    if (!draft.range()) throw std::invalid_argument("an event schedule is required");
    // Reject known wrong-account and read-only targets before changing session state.
    const auto target = calendar(calendarId);
    if (!target || target->accountId != accountId)
        throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "calendar does not belong to account");
    if (target->remote.readOnly)
        throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "calendar is read-only");
    Event saved;
    impl_->run(accountId, [&](const Impl::Session &session) {
        const auto found = session.objects.find(calendarId);
        if (found == session.objects.end())
            throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "calendar does not belong to account");
        if (found->second.remote.readOnly)
            throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "calendar is read-only");
        if (found->second.events.size() >= impl_->limits.maximumEvents)
            throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "event count limit exceeded");
        std::size_t total = 0;
        for (const auto &[id, object] : session.objects) { (void)id; total += object.events.size(); }
        if (total >= impl_->limits.maximumSnapshotEvents)
            throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "account event snapshot exceeds limit");
        impl_->requireCurrent(session);
        return impl_->projectEvent(session.provider->createEvent(found->second.remote, draft), found->second);
    }, [&](Impl::Session &session, Event result) {
        auto &object = session.objects.at(calendarId);
        if (std::any_of(object.events.begin(), object.events.end(), [&](const auto &item) { return item.id == result.id; }))
            invalid("created event has a duplicate id");
        saved = result;
        if (object.loadedWindow && result.status != EventStatus::Cancelled && result.range()->overlaps(*object.loadedWindow)) {
            object.events.push_back(std::move(result));
            std::sort(object.events.begin(), object.events.end(), [](const Event &a, const Event &b) {
                return std::pair(a.range()->start, a.id) < std::pair(b.range()->start, b.id);
            });
        }
    }, {}, true);
    return saved;
}
Event CalendarObjectManager::updateOccurrence(const std::string &accountId, const std::string &calendarId,
                                              const std::string &eventId, const EventOccurrenceUpdate &change) {
    const auto target = calendar(calendarId);
    if (!target || target->accountId != accountId)
        throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "calendar does not belong to account");
    if (target->remote.readOnly)
        throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "calendar is read-only");
    Event saved;
    impl_->run(accountId, [&](const Impl::Session &session) {
        const auto found = session.objects.find(calendarId);
        if (found == session.objects.end()) throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "calendar is unavailable");
        if (found->second.remote.readOnly) throw ExternalCalendarError(ExternalCalendarErrorCode::PermissionDenied, "calendar is read-only");
        const auto item = std::find_if(found->second.events.begin(), found->second.events.end(), [&](const Event &e) { return e.id == eventId; });
        if (item == found->second.events.end()) throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "occurrence is unavailable");
        if (item->kind != EventKind::Event) throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "only events can be edited");
        auto candidate = *item; candidate.title = change.title; candidate.schedule = change.schedule;
        candidate.validate(impl_->registry);
        if (!candidate.range()) throw std::invalid_argument("an event schedule is required");
        auto original = *item;
        original.id = original.properties.at("external.event");
        impl_->requireCurrent(session);
        auto result = impl_->projectEvent(session.provider->updateOccurrence(found->second.remote, original, change), found->second);
        if (result.kind != EventKind::Event || (change.title != original.title && result.title != change.title))
            invalid("provider did not confirm the requested occurrence title");
        if (change.schedule != original.schedule) {
            bool matches = false;
            if (const auto *requested = std::get_if<TimedSchedule>(&change.schedule)) {
                const auto *actual = std::get_if<TimedSchedule>(&result.schedule);
                matches = actual && requested->range == actual->range;
            } else if (const auto *requested = std::get_if<AllDaySchedule>(&change.schedule)) {
                const auto *actual = std::get_if<AllDaySchedule>(&result.schedule);
                matches = actual && requested->start == actual->start && requested->end == actual->end;
            }
            if (!matches) invalid("provider did not confirm the requested occurrence time");
        }
        return result;
    }, [&](Impl::Session &session, Event result) {
        auto &object = session.objects.at(calendarId);
        auto item = std::find_if(object.events.begin(), object.events.end(), [&](const Event &e) { return e.id == eventId; });
        if (item == object.events.end()) invalid("updated occurrence is missing");
        if (std::any_of(object.events.begin(), object.events.end(), [&](const Event &e) { return e.id == result.id && e.id != eventId; }))
            invalid("updated occurrence has a duplicate id");
        saved = result;
        if (object.loadedWindow && result.status != EventStatus::Cancelled && result.range()->overlaps(*object.loadedWindow)) *item = std::move(result);
        else object.events.erase(item);
        std::sort(object.events.begin(), object.events.end(), [](const Event &a, const Event &b) {
            return std::pair(a.range()->start, a.id) < std::pair(b.range()->start, b.id);
        });
    }, {}, true);
    return saved;
}
std::vector<RemoteTaskList> CalendarObjectManager::taskLists(const std::string &accountId) {
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->sessions.find(accountId);
        if (impl_->retired || found == impl_->sessions.end())
            throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "account is not logged in");
        // EventKit's global default can change in system settings at any time.
        if (found->second->taskListsLoaded && found->second->snapshot.account.providerId != "apple") return found->second->taskLists;
    }
    std::vector<RemoteTaskList> result;
    impl_->run(accountId, [&](const Impl::Session &session) {
        impl_->requireCurrent(session);
        auto lists = session.provider->taskLists();
        if (lists.size() > impl_->limits.maximumCalendars)
            throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "task list count limit exceeded");
        std::set<std::string> ids; unsigned defaults = 0;
        for (const auto &list : lists) {
            if (list.id.empty() || !ids.insert(list.id).second) invalid("missing or duplicate task list id");
            if (list.requiresConfirmation && (list.isDefault || list.sourceId.empty())) invalid("invalid unverified task source");
            if (list.isDefault) ++defaults;
        }
        if (defaults > 1) invalid("more than one default task list");
        return lists;
    }, [&](Impl::Session &session, std::vector<RemoteTaskList> lists) {
        session.taskLists = std::move(lists); session.taskListsLoaded = true; result = session.taskLists;
    }, {}, true);
    return result;
}
std::vector<RemoteTask> CalendarObjectManager::readTasks(const std::string &accountId) {
    struct TaskRead { std::vector<RemoteTask> tasks; std::vector<RemoteTaskList> destinations; };
    std::vector<RemoteTask> result;
    impl_->run(accountId, [&](const Impl::Session &session) {
        impl_->requireCurrent(session);
        auto lists = session.provider->allTaskLists();
        if (lists.size() > impl_->limits.maximumCalendars)
            throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "task list count limit exceeded");
        std::set<std::string> listIds;
        TaskRead snapshot;
        for (const auto &list : lists) {
            if (list.id.empty() || !listIds.insert(list.id).second || list.requiresConfirmation)
                invalid("invalid task list identity");
            if (session.snapshot.account.providerId == "microsoft" && list.isDefault) snapshot.destinations.push_back(list);
            impl_->requireCurrent(session);
            auto entries = session.provider->tasks(list);
            if (entries.size() > impl_->limits.maximumEvents ||
                entries.size() > impl_->limits.maximumSnapshotEvents - snapshot.tasks.size())
                throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "task count limit exceeded");
            std::set<std::string> taskIds;
            for (auto &task : entries) {
                if (task.id.empty() || !taskIds.insert(task.id).second) invalid("invalid task identity");
                task.remoteId = task.id;
                task.id = "external-task:" + component(accountId) + component(list.id) + component(task.remoteId);
                task.accountId = accountId; task.listId = list.id; task.listTitle = list.title;
                snapshot.tasks.push_back(std::move(task));
            }
        }
        if (snapshot.destinations.size() > 1) invalid("more than one default task list");
        return snapshot;
    }, [&](Impl::Session &session, TaskRead snapshot) {
        // Graph identifies the writable default in its list response; reuse it
        // for later creation without repeating the account list request.
        if (!snapshot.destinations.empty()) {
            session.taskLists = std::move(snapshot.destinations);
            session.taskListsLoaded = true;
        }
        result = std::move(snapshot.tasks);
    }, {}, true);
    return result;
}
Event CalendarObjectManager::createTask(const std::string &accountId, const std::string &calendarId,
                                        const std::string &taskListId, Event draft, bool confirmUnverifiedSource) {
    if (draft.kind != EventKind::Task || draft.calendarId != calendarId)
        throw std::invalid_argument("a non-recurring task for the selected account is required");
    if (draft.recurrence != RecurrenceRule{})
        throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "recurring task creation is not supported yet");
    if (draft.id.empty()) draft.id = "draft";
    draft.validate(impl_->registry);
    if (!draft.range()) throw std::invalid_argument("a task schedule is required");
    const auto target = calendar(calendarId);
    if (!target || target->accountId != accountId)
        throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "calendar does not belong to account");
    Event saved;
    impl_->run(accountId, [&](const Impl::Session &session) {
        const auto calendar = session.objects.find(calendarId);
        if (calendar == session.objects.end())
            throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "calendar does not belong to account");
        const auto list = std::find_if(session.taskLists.begin(), session.taskLists.end(), [&](const auto &item) { return item.id == taskListId; });
        if (!session.taskListsLoaded || list == session.taskLists.end())
            throw ExternalCalendarError(ExternalCalendarErrorCode::NotFound, "task list does not belong to account");
        if (list->requiresConfirmation && !confirmUnverifiedSource)
            throw ExternalCalendarError(ExternalCalendarErrorCode::Unsupported, "task source requires explicit confirmation");
        impl_->requireCurrent(session);
        auto result = session.provider->createTask(*list, draft, confirmUnverifiedSource);
        if (result.id.empty() || result.kind != EventKind::Task || !result.range()) invalid("invalid created task");
        result.properties["external.provider"] = session.snapshot.account.providerId;
        result.properties["external.account"] = accountId;
        result.properties["external.taskList"] = list->id;
        result.properties["external.task"] = result.id;
        result.id = "external-task:" + component(accountId) + component(list->id) + component(result.id);
        result.calendarId = calendarId; result.ownerId = accountId; result.revision = 0;
        try { result.validate(impl_->registry); } catch (...) { invalid("invalid created task"); }
        return result;
    }, [&](Impl::Session &, Event result) { saved = std::move(result); }, {}, true);
    return saved;
}
bool CalendarObjectManager::logout(const std::string &accountId) {
    std::lock_guard lock(impl_->mutex);
    return impl_->sessions.erase(accountId) != 0;
}
void CalendarObjectManager::retire() {
    std::lock_guard lock(impl_->mutex);
    impl_->retired = true;
    impl_->sessions.clear();
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
} // namespace iiCalendarIntegration
