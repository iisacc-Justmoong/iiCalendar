#pragma once
#include "Date.h"
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace iiCalendar {
inline constexpr std::uint64_t AttosecondsPerSecond = 1'000'000'000'000'000'000ULL;

// Canonical floor representation: seconds + fraction/1e18; 0 <= fraction < 1e18.
class Duration {
  public:
    Duration(std::int64_t seconds = 0, std::uint64_t fraction = 0);
    static Duration fromNanoseconds(std::int64_t nanoseconds);
    std::int64_t seconds() const noexcept { return seconds_; }
    std::uint64_t fraction() const noexcept { return fraction_; }
    Duration operator+(Duration other) const;
    Duration operator-(Duration other) const;
    auto operator<=>(const Duration &) const = default;

  private:
    std::int64_t seconds_;
    std::uint64_t fraction_;
};

// POSIX/Unix epoch, no implicit leap second or TAI interpretation.
class Instant {
  public:
    Instant(std::int64_t unixSeconds = 0, std::uint64_t fraction = 0);
    static Instant parse(const std::string &rfc3339);
    std::string toString() const;
    std::int64_t seconds() const noexcept { return seconds_; }
    std::uint64_t fraction() const noexcept { return fraction_; }
    Instant operator+(Duration delta) const;
    Instant operator-(Duration delta) const;
    Duration operator-(Instant other) const;
    auto operator<=>(const Instant &) const = default;

  private:
    std::int64_t seconds_;
    std::uint64_t fraction_;
};

class Time {
  public:
    Time(unsigned hour = 0, unsigned minute = 0, unsigned second = 0, std::uint64_t fraction = 0);
    static Time fromSecondOfDay(unsigned second, std::uint64_t fraction = 0);
    static Time parse(const std::string &iso);
    unsigned hour() const noexcept { return secondOfDay_ / 3600; }
    unsigned minute() const noexcept { return secondOfDay_ / 60 % 60; }
    unsigned second() const noexcept { return secondOfDay_ % 60; }
    unsigned secondOfDay() const noexcept { return secondOfDay_; }
    std::uint64_t fraction() const noexcept { return fraction_; }
    std::string toString() const;
    auto operator<=>(const Time &) const = default;

  private:
    unsigned secondOfDay_;
    std::uint64_t fraction_;
};

struct LocalDateTime {
    Date date;
    Time time;
    auto operator<=>(const LocalDateTime &) const = default;
};
struct TimeRange {
    Instant start, end; // Half-open; equal bounds represent a point event.
    void validate() const;
    bool overlaps(TimeRange other) const;
    auto operator<=>(const TimeRange &) const = default;
};
struct ClockSample {
    Instant wallTime;
    std::optional<Duration> wallResolution; // OS-reported granularity; unknown on Windows.
    Duration monotonic;
    Duration samplingWindow; // Bracketing cost, NOT a UTC accuracy guarantee.
    std::string source;
};
class Clock {
  public:
    static ClockSample sample();
};
} // namespace iiCalendar
