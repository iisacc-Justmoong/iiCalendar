#include "Checked.h"
#include <iiCalendar/Time.h>
#include <iomanip>
#include <sstream>

namespace iiCalendar {
namespace {
void fractionCheck(std::uint64_t v) {
    if (v >= AttosecondsPerSecond)
        throw std::invalid_argument("fraction must be in [0, 1e18)");
}
Duration plus(Duration a, Duration b) {
    const auto fraction = a.fraction() + b.fraction();
    const bool carry = fraction >= AttosecondsPerSecond;
    auto x = a.seconds(), y = b.seconds();
    // Apply a carry to a negative operand first; an intermediate underflow may cancel.
    if (carry && x < 0)
        ++x;
    else if (carry && y < 0)
        ++y;
    else if (carry)
        return Duration(detail::add(detail::add(x, y), 1), fraction - AttosecondsPerSecond);
    return Duration(detail::add(x, y), fraction % AttosecondsPerSecond);
}
Duration minus(Duration a, Duration b) {
    auto x = a.seconds(), y = b.seconds();
    const bool borrow = a.fraction() < b.fraction();
    if (borrow) {
        if (x > std::numeric_limits<std::int64_t>::min())
            --x;
        else
            y = detail::add(y, 1);
    }
    return Duration(detail::sub(x, y), borrow ? AttosecondsPerSecond + a.fraction() - b.fraction()
                                              : a.fraction() - b.fraction());
}
unsigned digits(const std::string &s, std::size_t start, std::size_t count) {
    if (start + count > s.size())
        throw std::invalid_argument("short time field");
    unsigned value = 0;
    for (std::size_t i = start; i < start + count; ++i) {
        if (s[i] < '0' || s[i] > '9')
            throw std::invalid_argument("invalid time digit");
        value = value * 10 + static_cast<unsigned>(s[i] - '0');
    }
    return value;
}
} // namespace
Duration::Duration(std::int64_t seconds, std::uint64_t fraction)
    : seconds_(seconds), fraction_(fraction) {
    fractionCheck(fraction);
}
Instant::Instant(std::int64_t seconds, std::uint64_t fraction)
    : seconds_(seconds), fraction_(fraction) {
    fractionCheck(fraction);
}
Duration Duration::fromNanoseconds(std::int64_t ns) {
    return {detail::floorDiv(ns, 1'000'000'000),
            static_cast<std::uint64_t>(detail::mod(ns, 1'000'000'000)) * 1'000'000'000};
}
Duration Duration::operator+(Duration other) const {
    return plus(*this, other);
}
Duration Duration::operator-(Duration other) const {
    return minus(*this, other);
}
Instant Instant::operator+(Duration delta) const {
    auto d = plus({seconds_, fraction_}, delta);
    return {d.seconds(), d.fraction()};
}
Instant Instant::operator-(Duration delta) const {
    auto d = minus({seconds_, fraction_}, delta);
    return {d.seconds(), d.fraction()};
}
Duration Instant::operator-(Instant other) const {
    return minus({seconds_, fraction_}, {other.seconds_, other.fraction_});
}
Time::Time(unsigned h, unsigned m, unsigned s, std::uint64_t f) : secondOfDay_(0), fraction_(f) {
    if (h > 23 || m > 59 || s > 59)
        throw std::invalid_argument(
            "invalid time; leap seconds require an explicit time-scale conversion");
    fractionCheck(f);
    secondOfDay_ = h * 3600 + m * 60 + s;
}
Time Time::fromSecondOfDay(unsigned s, std::uint64_t f) {
    if (s >= 86400)
        throw std::invalid_argument("invalid second of day");
    return Time(s / 3600, s / 60 % 60, s % 60, f);
}
std::string Time::toString() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setfill('0') << std::setw(2) << hour() << ':' << std::setw(2) << minute() << ':'
        << std::setw(2) << second();
    if (fraction_) {
        std::ostringstream f;
        f.imbue(std::locale::classic());
        f << std::setfill('0') << std::setw(18) << fraction_;
        auto text = f.str();
        while (text.back() == '0')
            text.pop_back();
        out << '.' << text;
    }
    return out.str();
}
Time Time::parse(const std::string &iso) {
    if (iso.size() < 8 || iso[2] != ':' || iso[5] != ':')
        throw std::invalid_argument("invalid ISO time");
    std::uint64_t fraction = 0;
    if (iso.size() > 8) {
        if (iso[8] != '.' || iso.size() < 10 || iso.size() > 27)
            throw std::invalid_argument("time requires 1..18 fractional digits");
        for (std::size_t i = 9; i < iso.size(); ++i)
            fraction = fraction * 10 + digits(iso, i, 1);
        for (std::size_t i = iso.size() - 9; i < 18; ++i)
            fraction *= 10;
    }
    return Time(digits(iso, 0, 2), digits(iso, 3, 2), digits(iso, 6, 2), fraction);
}
Instant Instant::parse(const std::string &iso) {
    auto separator = iso.find('T');
    if (separator == std::string::npos)
        throw std::invalid_argument("RFC3339 requires T");
    auto date = Date::parse(iso.substr(0, separator));
    auto offsetPos = iso.find_first_of("Z+-", separator + 1);
    if (offsetPos == std::string::npos)
        throw std::invalid_argument("RFC3339 requires an offset");
    auto time = Time::parse(iso.substr(separator + 1, offsetPos - separator - 1));
    int offset = 0;
    if (iso[offsetPos] == 'Z') {
        if (offsetPos + 1 != iso.size())
            throw std::invalid_argument("trailing timestamp data");
    } else {
        if (iso.size() != offsetPos + 6 || iso[offsetPos + 3] != ':')
            throw std::invalid_argument("invalid UTC offset");
        auto h = digits(iso, offsetPos + 1, 2), m = digits(iso, offsetPos + 4, 2);
        if (h > 23 || m > 59)
            throw std::invalid_argument("invalid UTC offset");
        offset = static_cast<int>(h * 3600 + m * 60);
        if (iso[offsetPos] == '-') {
            if (offset == 0)
                throw std::invalid_argument("unknown -00:00 offset cannot identify an instant");
            offset = -offset;
        }
    }
    return Instant(date.epochDay() * 86400 + time.secondOfDay() - offset, time.fraction());
}
std::string Instant::toString() const {
    return Date::fromEpochDay(detail::floorDiv(seconds_, 86400)).toString() + "T" +
           Time::fromSecondOfDay(static_cast<unsigned>(detail::mod(seconds_, 86400)), fraction_)
               .toString() +
           "Z";
}
void TimeRange::validate() const {
    if (end < start)
        throw std::invalid_argument("reversed time range");
}
bool TimeRange::overlaps(TimeRange other) const {
    validate();
    other.validate();
    if (other.start == other.end)
        return false;
    if (start == end)
        return start >= other.start && start < other.end;
    return start < other.end && end > other.start;
}
} // namespace iiCalendar
