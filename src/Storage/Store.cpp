#include "Codec.h"
#include <algorithm>
#include <iiCalendar/Store.h>
#include <limits>
#include <sqlite3.h>

namespace iiCalendar {
namespace {
constexpr std::int64_t applicationId = 0x69494361;
void sqlCheck(sqlite3 *db, int result) {
    if (result != SQLITE_OK)
        throw StorageError(sqlite3_errmsg(db));
}
void exec(sqlite3 *db, const char *sql) {
    sqlCheck(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}
struct Statement {
    sqlite3 *db;
    sqlite3_stmt *stmt = nullptr;
    Statement(sqlite3 *connection, const std::string &sql) : db(connection) {
        sqlCheck(db, sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr));
    }
    ~Statement() { sqlite3_finalize(stmt); }
    Statement(const Statement &) = delete;
    void text(int i, const std::string &value) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::length_error("SQLite text too large");
        sqlCheck(db, sqlite3_bind_text(stmt, i, value.data(), static_cast<int>(value.size()),
                                       SQLITE_TRANSIENT));
    }
    void number(int i, std::int64_t value) { sqlCheck(db, sqlite3_bind_int64(stmt, i, value)); }
    void null(int i) { sqlCheck(db, sqlite3_bind_null(stmt, i)); }
    bool step() {
        auto rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW)
            return true;
        if (rc == SQLITE_DONE)
            return false;
        throw StorageError(sqlite3_errmsg(db));
    }
};
struct Transaction {
    sqlite3 *db;
    bool committed = false;
    explicit Transaction(sqlite3 *value) : db(value) { exec(db, "BEGIN IMMEDIATE"); }
    ~Transaction() {
        if (!committed)
            (void)sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        exec(db, "COMMIT");
        committed = true;
    }
};
std::int64_t scalar(sqlite3 *db, const char *sql) {
    Statement s(db, sql);
    if (!s.step())
        throw StorageError("scalar query returned no row");
    return sqlite3_column_int64(s.stmt, 0);
}
std::string stringColumn(sqlite3_stmt *statement, int col) {
    const auto *value = sqlite3_column_text(statement, col);
    return value ? std::string(reinterpret_cast<const char *>(value),
                               static_cast<std::size_t>(sqlite3_column_bytes(statement, col)))
                 : std::string();
}
void bindInstant(Statement &s, int index, std::optional<Instant> instant) {
    if (instant) {
        s.number(index, instant->seconds());
        s.number(index + 1, static_cast<std::int64_t>(instant->fraction()));
    } else {
        s.null(index);
        s.null(index + 1);
    }
}
const char *schema = R"SQL(
CREATE TABLE events (
 id TEXT PRIMARY KEY NOT NULL, calendar_id TEXT NOT NULL,
 kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 4), status INTEGER NOT NULL CHECK(status BETWEEN 0 AND 4),
 title TEXT NOT NULL, revision INTEGER NOT NULL CHECK(revision>0), recurring INTEGER NOT NULL CHECK(recurring IN(0,1)),
 start_sec INTEGER, start_sub INTEGER, end_sec INTEGER, end_sub INTEGER,
 local_day INTEGER, local_second INTEGER, local_sub INTEGER, zone TEXT NOT NULL,
 all_day INTEGER NOT NULL CHECK(all_day IN(0,1)), due_sec INTEGER, due_sub INTEGER,
 updated_sec INTEGER NOT NULL, updated_sub INTEGER NOT NULL, payload BLOB NOT NULL,
 CHECK(start_sub IS NULL OR start_sub BETWEEN 0 AND 999999999999999999),
 CHECK(end_sub IS NULL OR end_sub BETWEEN 0 AND 999999999999999999),
 CHECK(local_sub IS NULL OR local_sub BETWEEN 0 AND 999999999999999999),
 CHECK(due_sub IS NULL OR due_sub BETWEEN 0 AND 999999999999999999),
 CHECK(updated_sub BETWEEN 0 AND 999999999999999999),
 CHECK(local_second IS NULL OR local_second BETWEEN 0 AND 86399),
 CHECK((start_sec IS NULL)=(start_sub IS NULL)), CHECK((end_sec IS NULL)=(end_sub IS NULL)),
 CHECK((start_sec IS NULL)=(end_sec IS NULL)), CHECK((start_sec IS NULL)=(local_day IS NULL)),
 CHECK((local_day IS NULL)=(local_second IS NULL)), CHECK((local_day IS NULL)=(local_sub IS NULL)),
 CHECK((due_sec IS NULL)=(due_sub IS NULL)),
 CHECK(start_sec IS NULL OR (end_sec,end_sub)>=(start_sec,start_sub))
) STRICT;
CREATE TABLE event_tags (event_id TEXT NOT NULL REFERENCES events(id) ON DELETE CASCADE, tag TEXT NOT NULL,
 PRIMARY KEY(event_id,tag)) STRICT;
CREATE INDEX idx_events_start ON events(start_sec,start_sub,id);
CREATE INDEX idx_events_end ON events(end_sec,end_sub,id);
CREATE INDEX idx_events_calendar_start ON events(calendar_id,start_sec,start_sub,id);
CREATE INDEX idx_events_date_time ON events(local_day,local_second,local_sub,id);
CREATE INDEX idx_events_calendar_date ON events(calendar_id,local_day,local_second,local_sub,id);
CREATE INDEX idx_events_kind_status ON events(kind,status,start_sec,start_sub);
CREATE INDEX idx_events_due ON events(due_sec,due_sub) WHERE due_sec IS NOT NULL;
CREATE INDEX idx_events_recurring ON events(calendar_id,start_sec,start_sub) WHERE recurring=1;
CREATE INDEX idx_tags_lookup ON event_tags(tag,event_id);
PRAGMA user_version=1;
PRAGMA application_id=1766409057;
)SQL";
} // namespace
struct Store::Impl {
    std::shared_ptr<sqlite3> db;
    std::shared_ptr<const CalendarRegistry> calendars;
    mutable std::mutex mutex;
    Event read(sqlite3_stmt *row) const {
        try {
            auto *data = static_cast<const std::byte *>(sqlite3_column_blob(row, 0));
            auto size = sqlite3_column_bytes(row, 0);
            auto event = detail::decode(std::span(data, static_cast<std::size_t>(size)));
            event.validate(*calendars);
            if (event.id != stringColumn(row, 1) ||
                event.revision != static_cast<std::uint64_t>(sqlite3_column_int64(row, 2)))
                throw StorageError("event index/payload mismatch");
            return event;
        } catch (const std::exception &e) {
            throw StorageError(std::string("invalid stored event: ") + e.what());
        }
    }
};
Store Store::inMemory(std::shared_ptr<const CalendarRegistry> calendars) {
    if (!calendars)
        throw std::invalid_argument("calendar registry is required");
    sqlite3 *raw = nullptr;
    const int opened = sqlite3_open_v2(":memory:", &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> owned(raw, sqlite3_close_v2);
    if (opened != SQLITE_OK)
        throw StorageError(raw ? sqlite3_errmsg(raw) : "cannot allocate SQLite connection");
    return Store(std::shared_ptr<sqlite3>(std::move(owned)), std::move(calendars));
}

Store::Store(std::shared_ptr<sqlite3> connection, std::shared_ptr<const CalendarRegistry> calendars)
    : impl_(std::make_unique<Impl>()) {
    if (!calendars)
        throw std::invalid_argument("calendar registry is required");
    if (!connection)
        throw std::invalid_argument("SQLite connection is required");
    impl_->calendars = std::move(calendars);
    impl_->db = std::move(connection);
    auto *raw = impl_->db.get();
    sqlCheck(raw, sqlite3_busy_timeout(raw, 5000));
    exec(raw, "PRAGMA foreign_keys=ON");
    // Serialize schema discovery with writers. Never claim a pre-existing unrelated DB.
    {
        Transaction tx(raw);
        auto app = scalar(raw, "PRAGMA application_id"),
             version = scalar(raw, "PRAGMA user_version");
        if (app == 0 && version == 0) {
            if (scalar(raw, "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'") !=
                0)
                throw StorageError("database belongs to another application");
            exec(raw, schema);
        } else if (app != applicationId || version != 1)
            throw StorageError("unsupported database application/schema version");
        tx.commit();
    }
    exec(raw, "PRAGMA journal_mode=WAL");
    exec(raw, "PRAGMA synchronous=FULL");
}
Store::~Store() = default;
Event Store::put(Event event, std::optional<std::uint64_t> expected) {
    event.validate(*impl_->calendars);
    auto range = event.range();
    auto local = event.localStart();
    std::lock_guard lock(impl_->mutex);
    auto *db = impl_->db.get();
    Transaction tx(db);
    std::int64_t previous = 0;
    {
        Statement s(db, "SELECT revision FROM events WHERE id=?");
        s.text(1, event.id);
        if (s.step())
            previous = sqlite3_column_int64(s.stmt, 0);
    }
    if (expected && *expected != static_cast<std::uint64_t>(previous))
        throw RevisionConflict("event revision changed");
    if (previous == std::numeric_limits<std::int64_t>::max())
        throw StorageError("event revision exhausted");
    event.revision = static_cast<std::uint64_t>(previous + 1);
    auto payload = detail::encode(event);
    Statement s(db, R"SQL(INSERT INTO events VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
     ON CONFLICT(id) DO UPDATE SET calendar_id=excluded.calendar_id,kind=excluded.kind,status=excluded.status,title=excluded.title,
     revision=excluded.revision,recurring=excluded.recurring,start_sec=excluded.start_sec,start_sub=excluded.start_sub,
     end_sec=excluded.end_sec,end_sub=excluded.end_sub,local_day=excluded.local_day,local_second=excluded.local_second,
     local_sub=excluded.local_sub,zone=excluded.zone,all_day=excluded.all_day,due_sec=excluded.due_sec,due_sub=excluded.due_sub,
     updated_sec=excluded.updated_sec,updated_sub=excluded.updated_sub,payload=excluded.payload)SQL");
    s.text(1, event.id);
    s.text(2, event.calendarId);
    s.number(3, static_cast<int>(event.kind));
    s.number(4, static_cast<int>(event.status));
    s.text(5, event.title);
    s.number(6, previous + 1);
    s.number(7,
             event.recurrence.frequency != Frequency::None || !event.recurrence.additional.empty());
    bindInstant(s, 8, range ? std::optional(range->start) : std::nullopt);
    bindInstant(s, 10, range ? std::optional(range->end) : std::nullopt);
    if (local) {
        s.number(12, local->date.epochDay());
        s.number(13, local->time.secondOfDay());
        s.number(14, static_cast<std::int64_t>(local->time.fraction()));
    } else {
        s.null(12);
        s.null(13);
        s.null(14);
    }
    s.text(15, event.zone());
    s.number(16, std::holds_alternative<AllDaySchedule>(event.schedule));
    bindInstant(s, 17, event.due);
    bindInstant(s, 19, event.updated);
    sqlCheck(db, sqlite3_bind_blob(s.stmt, 21, payload.data(), static_cast<int>(payload.size()),
                                   SQLITE_TRANSIENT));
    (void)s.step();
    {
        Statement clear(db, "DELETE FROM event_tags WHERE event_id=?");
        clear.text(1, event.id);
        (void)clear.step();
    }
    for (const auto &tag : event.tags) {
        Statement insert(db, "INSERT INTO event_tags VALUES (?,?)");
        insert.text(1, event.id);
        insert.text(2, tag);
        (void)insert.step();
    }
    tx.commit();
    return event;
}
std::optional<Event> Store::get(const std::string &id) const {
    std::lock_guard lock(impl_->mutex);
    Statement s(impl_->db.get(), "SELECT payload,id,revision FROM events WHERE id=?");
    s.text(1, id);
    if (!s.step())
        return {};
    return impl_->read(s.stmt);
}
bool Store::erase(const std::string &id, std::optional<std::uint64_t> expected) {
    std::lock_guard lock(impl_->mutex);
    auto *db = impl_->db.get();
    Transaction tx(db);
    std::optional<std::uint64_t> revision;
    {
        Statement s(db, "SELECT revision FROM events WHERE id=?");
        s.text(1, id);
        if (s.step())
            revision = static_cast<std::uint64_t>(sqlite3_column_int64(s.stmt, 0));
    }
    if (expected && *expected != revision.value_or(0))
        throw RevisionConflict("event revision changed");
    if (!revision) {
        tx.commit();
        return false;
    }
    Statement s(db, "DELETE FROM events WHERE id=?");
    s.text(1, id);
    (void)s.step();
    tx.commit();
    return true;
}
std::vector<Event> Store::query(const EventQuery &q) const {
    if (q.limit == 0 || q.limit > 10000 ||
        q.offset > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("invalid query pagination");
    if (q.window) {
        q.window->validate();
        if (q.window->start == q.window->end)
            return {};
    }
    std::string sql = "SELECT payload,id,revision FROM events WHERE 1=1";
    if (q.window)
        sql += " AND (start_sec,start_sub)<(?,?) AND ((end_sec,end_sub)>(?,?) OR "
               "((start_sec,start_sub)=(end_sec,end_sub) AND (start_sec,start_sub)>=(?,?)))";
    if (q.calendarId)
        sql += " AND calendar_id=?";
    if (q.kind)
        sql += " AND kind=?";
    if (q.status)
        sql += " AND status=?";
    if (q.tag)
        sql += " AND id IN (SELECT event_id FROM event_tags WHERE tag=?)";
    sql += " ORDER BY start_sec,start_sub,id LIMIT ? OFFSET ?";
    std::lock_guard lock(impl_->mutex);
    Statement s(impl_->db.get(), sql);
    int index = 1;
    if (q.window) {
        bindInstant(s, index, q.window->end);
        index += 2;
        bindInstant(s, index, q.window->start);
        index += 2;
        bindInstant(s, index, q.window->start);
        index += 2;
    }
    if (q.calendarId)
        s.text(index++, *q.calendarId);
    if (q.kind)
        s.number(index++, static_cast<int>(*q.kind));
    if (q.status)
        s.number(index++, static_cast<int>(*q.status));
    if (q.tag)
        s.text(index++, *q.tag);
    s.number(index++, static_cast<std::int64_t>(q.limit));
    s.number(index, static_cast<std::int64_t>(q.offset));
    std::vector<Event> result;
    while (s.step())
        result.push_back(impl_->read(s.stmt));
    return result;
}
std::vector<Event> Store::onLocalDate(Date date, const std::optional<std::string> &calendar) const {
    std::lock_guard lock(impl_->mutex);
    Statement s(impl_->db.get(), "SELECT payload,id,revision FROM events WHERE local_day=?" +
                                     std::string(calendar ? " AND calendar_id=?" : "") +
                                     " ORDER BY local_second,local_sub,id");
    s.number(1, date.epochDay());
    if (calendar)
        s.text(2, *calendar);
    std::vector<Event> result;
    while (s.step()) {
        if (result.size() >= 10000)
            throw std::length_error("local date query exceeds 10000 events");
        result.push_back(impl_->read(s.stmt));
    }
    return result;
}
std::vector<ScheduledEvent> Store::scheduled(TimeRange window,
                                             const std::optional<std::string> &calendar,
                                             ExpansionLimits limits) const {
    window.validate();
    if (window.start == window.end)
        return {};
    // Read each payload once under one SQLite statement snapshot; avoid N+1 reads in dayDetails.
    std::vector<Event> candidates;
    {
        std::lock_guard lock(impl_->mutex);
        std::string sql =
            "SELECT payload,id,revision FROM events WHERE status<>4 AND (recurring=1 OR "
            "((start_sec,start_sub)<(?,?) AND ((end_sec,end_sub)>(?,?) OR "
            "((start_sec,start_sub)=(end_sec,end_sub) AND (start_sec,start_sub)>=(?,?)))))";
        if (calendar)
            sql += " AND calendar_id=?";
        Statement s(impl_->db.get(), sql);
        bindInstant(s, 1, window.end);
        bindInstant(s, 3, window.start);
        bindInstant(s, 5, window.start);
        if (calendar)
            s.text(7, *calendar);
        while (s.step()) {
            if (candidates.size() >= 10000)
                throw std::length_error("agenda candidate limit exceeded");
            candidates.push_back(impl_->read(s.stmt));
        }
    }
    std::vector<ScheduledEvent> result;
    for (const auto &event : candidates)
        for (const auto &occurrence : expand(event, window, *impl_->calendars, limits)) {
            if (result.size() >= limits.maximumOccurrences)
                throw std::length_error("agenda occurrence limit exceeded");
            result.push_back({event, occurrence});
        }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return a.occurrence.range.start != b.occurrence.range.start
                   ? a.occurrence.range.start < b.occurrence.range.start
                   : a.event.id < b.event.id;
    });
    return result;
}
std::vector<Occurrence> Store::occurrences(TimeRange window,
                                           const std::optional<std::string> &calendar,
                                           ExpansionLimits limits) const {
    std::vector<Occurrence> result;
    for (auto &entry : scheduled(window, calendar, limits))
        result.push_back(std::move(entry.occurrence));
    return result;
}
} // namespace iiCalendar
