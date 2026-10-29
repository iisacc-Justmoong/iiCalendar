#include <algorithm>
#include <cmath>
#include <future>
#include <iiCalendar/iiCalendar.h>
#if defined(IICALENDAR_FILE_PROVIDER_TEST)
#include "FileProviderAdapter.h"
#include <iiFileProviderNative.h>
#endif
#include <iostream>
#include <limits>
#include <sqlite3.h>
#include <thread>

using namespace iiCalendar;
namespace {
int checks = 0;
void check(bool value, const char *expression, int line) {
    ++checks;
    if (!value)
        throw std::runtime_error(std::string(expression) + " at line " + std::to_string(line));
}
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)
template <class Exception = std::exception, class F> void fails(F f) {
    bool caught = false;
    try {
        f();
    } catch (const Exception &) {
        caught = true;
    }
    CHECK(caught);
}
Instant at(const std::string &value) {
    return Instant::parse(value);
}
Event example(std::string id = "review") {
    Event e;
    e.id = std::move(id);
    e.title = "Product review";
    e.schedule = TimedSchedule{{at("2026-09-19T00:30:00.000000000000000001Z"),
                                at("2026-09-19T01:15:00.000000000000000001Z")},
                               "Asia/Seoul"};
    e.created = at("2026-09-18T01:00:00Z");
    e.updated = e.created;
    return e;
}
void precision() {
    CHECK(Date(1970, 1, 1).epochDay() == 0);
    CHECK(Date(1969, 12, 31).epochDay() == -1);
    CHECK(Date(2000, 2, 29).addMonths(12) == Date(2001, 2, 28));
    fails([&] { Date(1900, 2, 29); });
    fails([&] { Date(2000, 2, 29).addMonths(12, MonthOverflow::Reject); });
    CHECK(Date(2021, 1, 1).weekDate() == WeekDate{2020, 53, 5});
    CHECK(Date::fromWeekDate({2020, 53, 5}) == Date(2021, 1, 1));
    fails([&] { Date::fromWeekDate({2021, 53, 1}); });
    for (int year : {-999999, -400, -1, 0, 1, 1582, 1900, 2000, 999999})
        for (unsigned month = 1; month <= 12; ++month) {
            Date date(year, month, Date::daysInMonth(year, month));
            CHECK(Date::fromEpochDay(date.epochDay()) == date);
            CHECK(Date::parse(date.toString()) == date);
        }
    CHECK(Duration::fromNanoseconds(-1) == Duration(-1, AttosecondsPerSecond - 1'000'000'000));
    CHECK(Instant(-1, AttosecondsPerSecond - 1) + Duration(0, 1) == Instant(0));
    CHECK(Instant(0) - Instant(0, 1) == Duration(-1, AttosecondsPerSecond - 1));
    CHECK(Duration(0, 1) - Duration(-1, AttosecondsPerSecond - 1) == Duration(0, 2));
    const auto hi = std::numeric_limits<std::int64_t>::max();
    const auto lo = std::numeric_limits<std::int64_t>::min();
    fails<std::overflow_error>(
        [&] { (void)(Instant(hi, AttosecondsPerSecond - 1) + Duration(0, 1)); });
    fails<std::overflow_error>([&] { (void)(Instant(lo) - Duration(0, 1)); });
    CHECK(Duration(lo, 1) - Duration(lo) == Duration(0, 1));
    CHECK(Duration(hi) + Duration(lo) == Duration(-1));
    CHECK(Duration(lo, AttosecondsPerSecond - 1) + Duration(-1, 1) == Duration(lo));
    CHECK(Duration(hi) - Duration(-1, 1) == Duration(hi, AttosecondsPerSecond - 1));
    const auto exact = at("1969-12-31T23:59:59.999999999999999999Z");
    CHECK(exact == Instant(-1, AttosecondsPerSecond - 1));
    CHECK(exact.toString() == "1969-12-31T23:59:59.999999999999999999Z");
    CHECK(at("2026-09-19T09:00:00.123456789012345678+09:00") ==
          at("2026-09-19T00:00:00.123456789012345678Z"));
    CHECK(Time::parse("23:59:59.000000000000000001").fraction() == 1);
    struct GroupedNumbers : std::numpunct<char> {
        char do_thousands_sep() const override { return ','; }
        std::string do_grouping() const override { return "\3"; }
    };
    auto oldLocale = std::locale::global(std::locale(std::locale::classic(), new GroupedNumbers));
    const auto stableFormat = Time(1, 2, 3, 123456789012345678).toString();
    std::locale::global(oldLocale);
    CHECK(stableFormat == "01:02:03.123456789012345678");
    fails([&] { Time::parse("23:59:60"); });
    fails([&] { at("2026-09-19T00:00:00.1234567890123456789Z"); });
    fails([&] { at("2026-09-19T00:00:00"); });
    fails([&] { at("2026-09-19T00:00:00-00:00"); });
    CHECK(TimeRange{Instant(1), Instant(2)}.overlaps({Instant(2), Instant(3)}) == false);
    CHECK(TimeRange{Instant(2), Instant(2)}.overlaps({Instant(2), Instant(3)}));
}
void calendars() {
    CalendarRegistry r;
    CHECK(r.ids().size() >= 18);
    CHECK(r.get("julian").toEpochDay({1582, 10, 4}) == Date(1582, 10, 14).epochDay());
    for (const auto &id : r.ids()) {
        const auto &c = r.get(id);
        for (Date date : {Date(1900, 1, 1), Date(2000, 2, 29), Date(2024, 2, 10), Date(2026, 9, 19),
                          Date(2030, 12, 31)}) {
            auto converted = c.fromEpochDay(date.epochDay());
            CHECK(c.toEpochDay(converted) == date.epochDay());
            auto next = c.addMonths(converted, 1, MonthOverflow::Clamp);
            CHECK(c.toEpochDay(next) > date.epochDay());
        }
    }
    const auto buddhist = r.get("buddhist").fromEpochDay(Date(2026, 9, 19).epochDay());
    CHECK(buddhist.yearOfEra == 2569);
    const auto persian = r.get("persian").fromEpochDay(Date(2024, 3, 20).epochDay());
    CHECK(persian.year == 1403 && persian.month == 1 && persian.day == 1);
    fails([&] { r.get("gregorian").toEpochDay({2026, 2, 30}); });
    fails([&] { r.get("hebrew").toEpochDay({5786, 1, 99}); });
    fails([&] { r.get("not-a-calendar"); });
    const auto &julian = r.get("julian");
    for (int year : {-999000, -1000, 0, 1900, 2000, 999000}) {
        auto day = julian.toEpochDay({year, 2, 28});
        auto date = julian.fromEpochDay(day);
        CHECK(date.year == year && date.month == 2 && date.day == 28);
    }
}
void zones() {
    TimeZone seoul("Asia/Seoul");
    auto instant = at("2026-09-19T00:30:00.123456789012345678Z");
    auto local = seoul.at(instant);
    CHECK(local.local.time.hour() == 9 && local.utcOffsetSeconds == 32400);
    CHECK(seoul.resolve(local.local) == instant);
    fails([&] { TimeZone("Invented/Timezone"); });
    TimeZone ny("America/New_York");
    LocalDateTime fold{Date(2026, 11, 1), Time(1, 30, 0, 1)};
    fails([&] { ny.resolve(fold); });
    auto first = ny.resolve(fold, AmbiguousTime::Earlier),
         last = ny.resolve(fold, AmbiguousTime::Later);
    CHECK(last - first == Duration(3600));
    CHECK(first.fraction() == 1 && last.fraction() == 1);
    LocalDateTime gap{Date(2026, 3, 8), Time(2, 30)};
    fails([&] { ny.resolve(gap); });
    CHECK(ny.at(ny.resolve(gap, AmbiguousTime::Earlier, MissingTime::ShiftForward)).local.time ==
          Time(3, 30));
    auto spring = ny.dayRange(Date(2026, 3, 8)), fall = ny.dayRange(Date(2026, 11, 1));
    CHECK(spring.end - spring.start == Duration(23 * 3600));
    CHECK(fall.end - fall.start == Duration(25 * 3600));
    auto skipped = TimeZone("Pacific/Apia").dayRange(Date(2011, 12, 30));
    CHECK(skipped.start == skipped.end);
    CHECK(!TimeZone::system().id().empty());
    CHECK(TimeZone::available().size() > 100);
    CHECK(!TimeZone::databaseVersion().empty());
}
void events() {
    CalendarRegistry r;
    auto e = example();
    e.validate(r);
    CHECK(e.localStart()->date == Date(2026, 9, 19));
    e.priority = 10;
    fails([&] { e.validate(r); });
    e.priority = 0;
    e.location.latitude = 91;
    e.location.longitude = 0;
    fails([&] { e.validate(r); });
    e.location = {};
    e.percentComplete = 101;
    fails([&] { e.validate(r); });
    e.percentComplete = 0;
    e.schedule = AllDaySchedule{Date(2026, 3, 8), Date(2026, 3, 9), "America/New_York"};
    e.validate(r);
    CHECK(e.range()->end - e.range()->start == Duration(23 * 3600));
    e.schedule = AllDaySchedule{Date(2026, 9, 19), Date(2026, 9, 19), "UTC"};
    fails([&] { e.validate(r); });
    e.schedule = {};
    e.kind = EventKind::Task;
    e.due = at("2026-09-19T01:00:00Z");
    e.validate(r);
    e.recurrence.frequency = Frequency::Daily;
    fails([&] { e.validate(r); });
}
void recurrence() {
    CalendarRegistry r;
    auto e = example();
    e.schedule = TimedSchedule{{at("2026-03-07T14:00:00.000000000000000001Z"),
                                at("2026-03-07T15:00:00.000000000000000001Z")},
                               "America/New_York"};
    e.recurrence.frequency = Frequency::Daily;
    e.recurrence.count = 3;
    auto window = TimeRange{at("2026-03-01T00:00:00Z"), at("2026-04-01T00:00:00Z")};
    auto result = expand(e, window, r);
    CHECK(result.size() == 3);
    CHECK(result[1].range.start == at("2026-03-08T13:00:00.000000000000000001Z"));
    CHECK(result[1].range.end - result[1].range.start == Duration(3600));
    e.recurrence.excluded = {result[1].range.start};
    e.recurrence.additional = {at("2026-03-12T13:00:00.000000000000000001Z")};
    result = expand(e, window, r);
    CHECK(result.size() == 3);
    e = example();
    e.schedule = TimedSchedule{{at("2026-01-31T09:00:00Z"), at("2026-01-31T10:00:00Z")}, "UTC"};
    e.recurrence.frequency = Frequency::Monthly;
    e.recurrence.count = 3;
    result = expand(e, {at("2026-01-01T00:00:00Z"), at("2026-07-01T00:00:00Z")}, r);
    CHECK(result.size() == 3);
    CHECK(result[1].localDate == Date(2026, 3, 31));
    CHECK(result[2].localDate == Date(2026, 5, 31));
    e = example();
    e.schedule = AllDaySchedule{Date(2026, 9, 7), Date(2026, 9, 8), "UTC"};
    e.recurrence.frequency = Frequency::Weekly;
    e.recurrence.weekdays = {{1, 0}, {3, 0}};
    e.recurrence.count = 4;
    result = expand(e, {at("2026-09-01T00:00:00Z"), at("2026-10-01T00:00:00Z")}, r);
    CHECK(result.size() == 4 && result[3].localDate == Date(2026, 9, 16));
    e.recurrence = {};
    e.recurrence.frequency = Frequency::Monthly;
    e.recurrence.weekdays = {{1, -1}};
    e.recurrence.count = 2;
    result = expand(e, {at("2026-09-01T00:00:00Z"), at("2026-11-01T00:00:00Z")}, r);
    CHECK(result.size() == 2 && result[0].localDate == Date(2026, 9, 28) &&
          result[1].localDate == Date(2026, 10, 26));
    fails(
        [&] { expand(e, {at("2026-09-01T00:00:00Z"), at("2026-11-01T00:00:00Z")}, r, {1, 100}); });
    fails(
        [&] { expand(e, {at("2026-09-01T00:00:00Z"), at("2026-11-01T00:00:00Z")}, r, {100, 1}); });
}
void foldStart() {
    CalendarRegistry r;
    auto e = example();
    auto later = TimeZone("America/New_York")
                     .resolve({Date(2026, 11, 1), Time(1, 30)}, AmbiguousTime::Later);
    e.schedule = TimedSchedule{{later, later + Duration(3600)}, "America/New_York"};
    e.recurrence.frequency = Frequency::Daily;
    e.recurrence.count = 2;
    auto result = expand(e, {at("2026-11-01T00:00:00Z"), at("2026-11-03T00:00:00Z")}, r);
    CHECK(result.size() == 2 && result[0].range.start == later);
}
void recurrenceMonthWork() {
    struct Counted final : CalendarSystem {
        const CalendarSystem &base;
        mutable unsigned calls=0;
        explicit Counted(const CalendarSystem &calendar):base(calendar) {}
        std::string id() const override { return "counted"; }
        CalendarDate fromEpochDay(std::int64_t day) const override { return base.fromEpochDay(day); }
        std::int64_t toEpochDay(CalendarDate day) const override { return base.toEpochDay(day); }
        CalendarDate addMonths(CalendarDate day,int months,MonthOverflow policy) const override {
            ++calls;return base.addMonths(day,months,policy);
        }
    };
    CalendarRegistry registry;
    auto counted=std::make_shared<Counted>(registry.get("gregorian"));
    registry.add(counted);
    auto event=example();
    event.schedule=AllDaySchedule{Date(2024,1,1),Date(2024,1,2),"UTC"};
    event.recurrence.frequency=Frequency::Daily;
    event.recurrence.monthDays={-1};
    const TimeRange window{at("2024-01-01T00:00:00Z"),at("2025-01-01T00:00:00Z")};
    const auto expected=expand(event,window,registry);
    event.calendarSystem="counted";
    const auto actual=expand(event,window,registry);
    CHECK(actual.size()==12 && actual.size()==expected.size());
    for (std::size_t i=0;i<actual.size();++i) {
        CHECK(actual[i].range==expected[i].range && actual[i].localDate==expected[i].localDate);
    }
    CHECK(actual[1].localDate==Date(2024,2,29));
    CHECK(counted->calls<=13);
}
void storageMemory() {
    {
        auto convenience = Store::inMemory();
        const auto saved = convenience.put(example("convenience"), 0);
        CHECK(convenience.get(saved.id) == saved);
    }
    fails<std::invalid_argument>([] { (void)Store::inMemory({}); });
    sqlite3 *raw = nullptr;
    const int opened = sqlite3_open_v2(":memory:", &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    std::shared_ptr<sqlite3> connection(raw, [](sqlite3 *value) { (void)sqlite3_close_v2(value); });
    CHECK(opened == SQLITE_OK && connection);
    {
        Store store(connection);
        auto saved = store.put(example("memory-event"), 0);
        CHECK(saved.revision == 1);
        CHECK(store.get(saved.id) == saved);
        CHECK(store.erase(saved.id, saved.revision));
        CHECK(!store.get(saved.id));
    }
    CHECK(sqlite3_exec(connection.get(), "SELECT 1", nullptr, nullptr, nullptr) == SQLITE_OK);
    fails<std::invalid_argument>([] { Store invalid(std::shared_ptr<sqlite3>{}); });
    fails<std::invalid_argument>([&] { Store invalid(connection, {}); });

    sqlite3 *otherRaw = nullptr;
    const int otherOpened = sqlite3_open(":memory:", &otherRaw);
    std::shared_ptr<sqlite3> other(otherRaw, [](sqlite3 *value) { (void)sqlite3_close_v2(value); });
    CHECK(otherOpened == SQLITE_OK && other);
    CHECK(sqlite3_exec(other.get(), "CREATE TABLE unrelated(id INTEGER)", nullptr, nullptr, nullptr) == SQLITE_OK);
    fails<StorageError>([&] { Store invalid(other); });
    CHECK(sqlite3_exec(other.get(), "INSERT INTO unrelated VALUES(1)", nullptr, nullptr, nullptr) == SQLITE_OK);
}
#if defined(IICALENDAR_FILE_PROVIDER_TEST)
void storage() {
    iiFileProvider::native::TemporaryDirectory directory;
    CHECK(directory.isValid());
    const auto path = directory.path() / "calendar-test.sqlite";
    auto e = example();
    e.description = "Unicode: 일정 · 🗓️";
    e.url = "https://iisacc.com";
    e.color = "#57965c";
    e.ownerId = "owner";
    e.kind = EventKind::Task;
    e.status = EventStatus::InProgress;
    e.priority = 1;
    e.percentComplete = 50;
    e.location = {"Studio", "Seoul", "geo:37.5,127.0", 37.5, 127.0};
    e.participants = {{"person", "Designer", "mailto:design@example.test",
                       ParticipantRole::Organizer, Participation::Accepted}};
    e.attachments = {{"file", "Launch brief.pdf", "file:///brief.pdf", "application/pdf", 248000}};
    e.alarms = {{Duration(900, 1), "display", "Review starts soon"}};
    e.tags = {"launch", "Society"};
    e.properties = {{"source", "calendar"}};
    e.links = {{"project", "society"}};
    e.due = at("2026-09-19T08:00:00.000000000000000002Z");
    {
        auto store = iiCalendarFileProvider::openStore(path);
        auto saved = store.put(e, 0);
        CHECK(saved.revision == 1);
        e = saved;
        CHECK(store.get(e.id) == e);
        auto oversized = e;
        for (int n = 0; n < 17; ++n)
            oversized.properties[std::to_string(n)] = std::string(1'048'576, 'x');
        fails<std::length_error>([&] { store.put(oversized, e.revision); });
        CHECK(store.get(e.id) == e);
        fails<RevisionConflict>([&] { store.put(e, 0); });
        e = store.put(e, 1);
        CHECK(e.revision == 2);
        fails<RevisionConflict>([&] { store.put(e, 1); });
        EventQuery q;
        q.tag = "launch";
        CHECK(store.query(q).size() == 1);
        q.tag = "absent";
        CHECK(store.query(q).empty());
        q = {};
        q.window = TimeRange{e.range()->end, e.range()->end + Duration(1)};
        CHECK(store.query(q).empty());
        q.window = TimeRange{e.range()->start, e.range()->start + Duration(0, 1)};
        CHECK(store.query(q).size() == 1);
        CHECK(store.onLocalDate(Date(2026, 9, 19)).size() == 1);
        auto boundary = example("boundary");
        boundary.schedule =
            TimedSchedule{{e.range()->start + Duration(0, 1), e.range()->end}, "Asia/Seoul"};
        store.put(boundary);
        CHECK(store.query(q).size() == 1);
        boundary.tags = {"a", "a"};
        fails([&] { store.put(boundary); });
        CHECK(store.get("boundary")->tags.empty());
        auto second = iiCalendarFileProvider::openStore(path);
        auto copy = second.get(e.id).value();
        copy.title = "other connection";
        copy = second.put(copy, e.revision);
        fails<RevisionConflict>([&] { store.put(e, e.revision); });
        e = copy;
        sqlite3 *raw = nullptr;
        CHECK(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
        CHECK(sqlite3_exec(raw,
                           "CREATE TRIGGER reject_test_tag BEFORE INSERT ON event_tags WHEN "
                           "NEW.tag='rollback-test' BEGIN SELECT RAISE(ABORT,'test rollback'); END",
                           nullptr, nullptr, nullptr) == SQLITE_OK);
        auto invalid = e;
        invalid.title = "must roll back";
        invalid.tags.push_back("rollback-test");
        fails<StorageError>([&] { store.put(invalid, e.revision); });
        CHECK(store.get(e.id) == e);
        CHECK(sqlite3_exec(raw, "DROP TRIGGER reject_test_tag", nullptr, nullptr, nullptr) ==
              SQLITE_OK);
        auto before = e;
        auto write = [&](Store &target, const char *title) {
            auto value = before;
            value.title = title;
            try {
                target.put(value, before.revision);
                return true;
            } catch (const RevisionConflict &) {
                return false;
            }
        };
        auto a = std::async(std::launch::async, [&] { return write(store, "writer A"); });
        auto b = std::async(std::launch::async, [&] { return write(second, "writer B"); });
        CHECK(static_cast<int>(a.get()) + static_cast<int>(b.get()) == 1);
        e = store.get(e.id).value();
        CHECK(sqlite3_exec(raw, "UPDATE events SET payload=x'0102' WHERE id='boundary'", nullptr,
                           nullptr, nullptr) == SQLITE_OK);
        fails<StorageError>([&] { store.get("boundary"); });
        boundary.tags.clear();
        store.put(boundary);
        sqlite3_close(raw);
    }
    {
        auto store = iiCalendarFileProvider::openStore(path);
        CHECK(store.get(e.id) == e);
        auto repeated = example("recurring");
        repeated.recurrence.frequency = Frequency::Daily;
        repeated.recurrence.count = 3;
        store.put(repeated);
        auto range = TimeZone("Asia/Seoul").dayRange(Date(2026, 9, 21));
        CHECK(store.occurrences(range).size() == 1);
        fails<RevisionConflict>([&] { store.erase(e.id, 1); });
        CHECK(store.erase(e.id, e.revision));
        CHECK(!store.get(e.id));
        CHECK(!store.erase(e.id));
    }
    sqlite3 *db = nullptr;
    CHECK(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    CHECK(sqlite3_prepare_v2(db,
                             "EXPLAIN QUERY PLAN SELECT id FROM events WHERE (start_sec,start_sub) "
                             ">= (1,0) ORDER BY start_sec,start_sub",
                             -1, &stmt, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    std::string plan(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3)));
    CHECK(plan.find("idx_events_start") != std::string::npos);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    CHECK(iiFileProvider::native::File::remove(path));
}
#endif
void views() {
    CalendarRegistry r;
    auto month =
        makeView({ViewKind::Month, Date(2026, 9, 19), "gregorian", 1, Date(2026, 9, 19)}, r);
    CHECK(month.days.size() == 42 && month.days.front().date == Date(2026, 8, 31));
    CHECK(std::count_if(month.days.begin(), month.days.end(), [](auto d) { return d.inPeriod; }) ==
          30);
    CHECK(std::count_if(month.days.begin(), month.days.end(), [](auto d) { return d.today; }) == 1);
    auto year = makeView({ViewKind::Year, Date(2024, 9, 19)}, r);
    CHECK(year.days.size() == 366);
    for (auto kind :
         {ViewKind::Day, ViewKind::Week, ViewKind::Agenda, ViewKind::Timeline, ViewKind::Custom}) {
        auto view = makeView({kind, Date(2026, 9, 19)}, r);
        CHECK(view.days.size() == (kind == ViewKind::Day ? 1 : 7));
    }
    auto chinese = makeView({ViewKind::Month, Date(2024, 2, 10), "chinese"}, r);
    CHECK(chinese.start == Date(2024, 2, 10));
    auto store = Store::inMemory();
    auto e = example();
    e.attachments = {{"f", "brief.pdf", "file:///brief.pdf", "application/pdf", 42}};
    store.put(e);
    auto task = e;
    task.id = "task";
    task.kind = EventKind::Task;
    task.status = EventStatus::Completed;
    task.percentComplete = 100;
    store.put(task);
    e.id = "second";
    store.put(e);
    auto details = dayDetails(store, Date(2026, 9, 19), TimeZone("Asia/Seoul"), 1);
    CHECK(details.events.total == 2 && details.events.preview.size() == 1 &&
          details.events.hasMore());
    CHECK(details.tasks.total == 1 && details.completedTasks == 1 && details.fileCount == 1);
}
void clockTest() {
    auto a = Clock::sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto b = Clock::sample();
    CHECK(b.monotonic >= a.monotonic);
    if (a.wallResolution)
        CHECK(*a.wallResolution > Duration());
    CHECK(a.samplingWindow >= Duration());
    auto approximate = std::chrono::system_clock::now().time_since_epoch();
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(approximate).count();
    CHECK(std::abs(b.wallTime.seconds() - sec) < 5);
    CHECK(!a.source.empty());
    std::cout << "clock=" << a.source;
    if (a.wallResolution)
        std::cout << " resolution=" << a.wallResolution->seconds() << "s + "
                  << a.wallResolution->fraction() << "as";
    std::cout << '\n';
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("one suite is required");
        const std::string suite = argv[1];
        if (suite == "precision")
            precision();
        else if (suite == "calendars")
            calendars();
        else if (suite == "zones")
            zones();
        else if (suite == "events")
            events();
        else if (suite == "recurrence") {
            recurrence();
            foldStart();
            recurrenceMonthWork();
        }
#if defined(IICALENDAR_FILE_PROVIDER_TEST)
        else if (suite == "storage")
            storage();
#endif
        else if (suite == "storage_memory")
            storageMemory();
        else if (suite == "views")
            views();
        else if (suite == "clock")
            clockTest();
        else
            throw std::runtime_error("unknown suite");
        std::cout << suite << ": " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
