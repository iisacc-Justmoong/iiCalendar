#include "Codec.h"
#include <bit>
#include <limits>

namespace iiCalendar::detail {
namespace {
constexpr std::size_t maxPayload = 16 * 1024 * 1024;
struct Writer {
    std::vector<std::byte> bytes;
    void u(std::uint64_t v) {
        if (bytes.size() > maxPayload - 8)
            throw std::length_error("event payload exceeds 16 MiB");
        for (unsigned i = 0; i < 8; ++i)
            bytes.push_back(static_cast<std::byte>((v >> (i * 8)) & 255));
    }
    void i(std::int64_t v) { u(std::bit_cast<std::uint64_t>(v)); }
    void s(const std::string &value) {
        u(value.size());
        if (value.size() > maxPayload - bytes.size())
            throw std::length_error("event payload exceeds 16 MiB");
        for (char ch : value)
            bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    void instant(Instant v) {
        i(v.seconds());
        u(v.fraction());
    }
    void duration(Duration v) {
        i(v.seconds());
        u(v.fraction());
    }
    void date(Date d) { i(d.epochDay()); }
    template <class T, class F> void optional(const std::optional<T> &value, F write) {
        u(value.has_value());
        if (value)
            write(*value);
    }
    template <class T, class F> void list(const T &values, F write) {
        u(values.size());
        for (const auto &value : values)
            write(value);
    }
};
struct Reader {
    std::span<const std::byte> bytes;
    std::size_t pos = 0;
    std::uint64_t u() {
        if (bytes.size() - pos < 8)
            throw std::invalid_argument("truncated event payload");
        std::uint64_t v = 0;
        for (unsigned i = 0; i < 8; ++i)
            v |= std::to_integer<std::uint64_t>(bytes[pos++]) << (i * 8);
        return v;
    }
    std::int64_t i() { return std::bit_cast<std::int64_t>(u()); }
    unsigned number() {
        auto n = u();
        if (n > std::numeric_limits<unsigned>::max())
            throw std::invalid_argument("payload number overflow");
        return static_cast<unsigned>(n);
    }
    int integer() {
        auto n = i();
        if (n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max())
            throw std::invalid_argument("payload integer overflow");
        return static_cast<int>(n);
    }
    bool boolean() {
        auto v = u();
        if (v > 1)
            throw std::invalid_argument("invalid payload boolean");
        return v != 0;
    }
    std::string s() {
        auto size = u();
        if (size > 1'048'576 || size > bytes.size() - pos)
            throw std::invalid_argument("invalid payload string length");
        std::string value(reinterpret_cast<const char *>(bytes.data() + pos),
                          static_cast<std::size_t>(size));
        pos += static_cast<std::size_t>(size);
        return value;
    }
    Instant instant() {
        auto sec = i();
        auto sub = u();
        return Instant(sec, sub);
    }
    Duration duration() {
        auto sec = i();
        auto sub = u();
        return Duration(sec, sub);
    }
    Date date() { return Date::fromEpochDay(i()); }
    template <class T, class F> std::optional<T> optional(F read) {
        if (!boolean())
            return {};
        return read();
    }
    template <class T, class F> std::vector<T> list(F read) {
        auto count = u();
        if (count > 10000)
            throw std::invalid_argument("payload collection too large");
        std::vector<T> values;
        values.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t j = 0; j < count; ++j)
            values.push_back(read());
        return values;
    }
    std::map<std::string, std::string> map() {
        std::map<std::string, std::string> out;
        auto pairs = list<std::pair<std::string, std::string>>([&] {
            auto k = s();
            auto v = s();
            return std::pair(std::move(k), std::move(v));
        });
        for (auto &[k, v] : pairs)
            if (!out.emplace(std::move(k), std::move(v)).second)
                throw std::invalid_argument("duplicate payload key");
        return out;
    }
};
} // namespace
std::vector<std::byte> encode(const Event &e) {
    Writer w;
    w.u(0x314C41434949ULL); // IICAL1, independent of host ABI/endianness.
    for (const auto *s : {&e.id, &e.calendarId, &e.calendarSystem, &e.title, &e.description, &e.url,
                          &e.color, &e.ownerId})
        w.s(*s);
    w.i(static_cast<int>(e.kind));
    w.i(static_cast<int>(e.status));
    w.i(static_cast<int>(e.visibility));
    w.u(e.priority);
    w.u(e.percentComplete);
    w.u(e.busy);
    w.u(e.schedule.index());
    if (auto *t = std::get_if<TimedSchedule>(&e.schedule)) {
        w.instant(t->range.start);
        w.instant(t->range.end);
        w.s(t->zone);
    }
    if (auto *d = std::get_if<AllDaySchedule>(&e.schedule)) {
        w.date(d->start);
        w.date(d->end);
        w.s(d->zone);
    }
    w.optional(e.due, [&](auto v) { w.instant(v); });
    w.optional(e.completed, [&](auto v) { w.instant(v); });
    w.instant(e.created);
    w.instant(e.updated);
    w.s(e.location.name);
    w.s(e.location.address);
    w.s(e.location.uri);
    w.optional(e.location.latitude, [&](double v) { w.u(std::bit_cast<std::uint64_t>(v)); });
    w.optional(e.location.longitude, [&](double v) { w.u(std::bit_cast<std::uint64_t>(v)); });
    w.list(e.participants, [&](const Participant &p) {
        w.s(p.id);
        w.s(p.name);
        w.s(p.address);
        w.i(static_cast<int>(p.role));
        w.i(static_cast<int>(p.response));
    });
    w.list(e.attachments, [&](const Attachment &a) {
        w.s(a.id);
        w.s(a.name);
        w.s(a.uri);
        w.s(a.mediaType);
        w.u(a.byteSize);
    });
    w.list(e.alarms, [&](const Alarm &a) {
        w.duration(a.beforeStart);
        w.s(a.action);
        w.s(a.description);
    });
    w.list(e.tags, [&](const auto &s) { w.s(s); });
    for (const auto *map : {&e.properties, &e.links})
        w.list(*map, [&](const auto &p) {
            w.s(p.first);
            w.s(p.second);
        });
    const auto &r = e.recurrence;
    w.i(static_cast<int>(r.frequency));
    w.u(r.interval);
    w.optional(r.count, [&](auto v) { w.u(v); });
    w.optional(r.until, [&](auto v) { w.instant(v); });
    w.list(r.weekdays, [&](auto v) {
        w.u(v.weekday);
        w.i(v.ordinal);
    });
    w.list(r.monthDays, [&](auto v) { w.i(v); });
    w.list(r.months, [&](auto v) { w.u(v); });
    w.u(r.weekStart);
    w.list(r.excluded, [&](auto v) { w.instant(v); });
    w.list(r.additional, [&](auto v) { w.instant(v); });
    w.u(e.revision);
    if (w.bytes.size() > maxPayload)
        throw std::length_error("event payload exceeds 16 MiB");
    return std::move(w.bytes);
}
Event decode(std::span<const std::byte> bytes) {
    if (bytes.size() > maxPayload)
        throw std::invalid_argument("event payload exceeds 16 MiB");
    Reader r{bytes};
    if (r.u() != 0x314C41434949ULL)
        throw std::invalid_argument("unsupported event payload version");
    Event e;
    for (auto *s : {&e.id, &e.calendarId, &e.calendarSystem, &e.title, &e.description, &e.url,
                    &e.color, &e.ownerId})
        *s = r.s();
    e.kind = static_cast<EventKind>(r.integer());
    e.status = static_cast<EventStatus>(r.integer());
    e.visibility = static_cast<Visibility>(r.integer());
    e.priority = r.number();
    e.percentComplete = r.number();
    e.busy = r.boolean();
    auto schedule = r.u();
    if (schedule == 1) {
        auto a = r.instant();
        auto b = r.instant();
        auto zone = r.s();
        e.schedule = TimedSchedule{{a, b}, zone};
    } else if (schedule == 2) {
        auto a = r.date();
        auto b = r.date();
        auto zone = r.s();
        e.schedule = AllDaySchedule{a, b, zone};
    } else if (schedule != 0)
        throw std::invalid_argument("invalid schedule type");
    e.due = r.optional<Instant>([&] { return r.instant(); });
    e.completed = r.optional<Instant>([&] { return r.instant(); });
    e.created = r.instant();
    e.updated = r.instant();
    e.location.name = r.s();
    e.location.address = r.s();
    e.location.uri = r.s();
    e.location.latitude = r.optional<double>([&] { return std::bit_cast<double>(r.u()); });
    e.location.longitude = r.optional<double>([&] { return std::bit_cast<double>(r.u()); });
    e.participants = r.list<Participant>([&] {
        Participant p;
        p.id = r.s();
        p.name = r.s();
        p.address = r.s();
        p.role = static_cast<ParticipantRole>(r.integer());
        p.response = static_cast<Participation>(r.integer());
        return p;
    });
    e.attachments = r.list<Attachment>([&] {
        Attachment a;
        a.id = r.s();
        a.name = r.s();
        a.uri = r.s();
        a.mediaType = r.s();
        a.byteSize = r.u();
        return a;
    });
    e.alarms = r.list<Alarm>([&] {
        auto d = r.duration();
        auto action = r.s();
        auto description = r.s();
        return Alarm{d, action, description};
    });
    e.tags = r.list<std::string>([&] { return r.s(); });
    e.properties = r.map();
    e.links = r.map();
    auto &rule = e.recurrence;
    rule.frequency = static_cast<Frequency>(r.integer());
    rule.interval = r.number();
    rule.count = r.optional<unsigned>([&] { return r.number(); });
    rule.until = r.optional<Instant>([&] { return r.instant(); });
    rule.weekdays = r.list<WeekdayRule>([&] {
        auto day = r.number();
        auto ordinal = r.integer();
        return WeekdayRule{day, ordinal};
    });
    rule.monthDays = r.list<int>([&] { return r.integer(); });
    rule.months = r.list<unsigned>([&] { return r.number(); });
    rule.weekStart = r.number();
    rule.excluded = r.list<Instant>([&] { return r.instant(); });
    rule.additional = r.list<Instant>([&] { return r.instant(); });
    e.revision = r.u();
    if (r.pos != bytes.size())
        throw std::invalid_argument("trailing event payload bytes");
    return e;
}
} // namespace iiCalendar::detail
