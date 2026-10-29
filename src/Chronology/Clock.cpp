#include <iiCalendar/Time.h>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <system_error>
#include <time.h>
#endif

namespace iiCalendar {
ClockSample Clock::sample() {
#ifdef _WIN32
    LARGE_INTEGER frequency{}, before{}, after{};
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&before))
        throw std::runtime_error("QueryPerformanceCounter failed");
    FILETIME wall{};
    GetSystemTimePreciseAsFileTime(&wall);
    if (!QueryPerformanceCounter(&after))
        throw std::runtime_error("QueryPerformanceCounter failed");
    auto ticks = (static_cast<std::uint64_t>(wall.dwHighDateTime) << 32) | wall.dwLowDateTime;
    constexpr std::uint64_t epoch = 116444736000000000ULL;
    if (ticks < epoch)
        throw std::runtime_error("OS clock predates Unix epoch");
    ticks -= epoch;
    // QPC converts via nanoseconds without overflowing ticks * 1e9.
    auto qpc = [&](LONGLONG count) {
        auto sec = count / frequency.QuadPart;
        auto remainder = count % frequency.QuadPart;
        const auto ns = static_cast<std::int64_t>(
            (static_cast<long double>(remainder) * 1'000'000'000) / frequency.QuadPart);
        return Duration(sec, static_cast<std::uint64_t>(ns) * 1'000'000'000);
    };
    return {Instant(static_cast<std::int64_t>(ticks / 10'000'000),
                    (ticks % 10'000'000) * 100'000'000'000ULL),
            std::nullopt, qpc(before.QuadPart), qpc(after.QuadPart) - qpc(before.QuadPart),
            "GetSystemTimePreciseAsFileTime / QPC (100 ns representation)"};
#else
    timespec before{}, wall{}, after{}, resolution{};
    auto read = [](clockid_t id, timespec &value) {
        if (clock_gettime(id, &value) != 0)
            throw std::system_error(errno, std::generic_category(), "clock_gettime");
    };
    read(CLOCK_MONOTONIC, before);
    read(CLOCK_REALTIME, wall);
    read(CLOCK_MONOTONIC, after);
    if (clock_getres(CLOCK_REALTIME, &resolution) != 0)
        throw std::system_error(errno, std::generic_category(), "clock_getres");
    auto duration = [](timespec v) {
        return Duration(static_cast<std::int64_t>(v.tv_sec),
                        static_cast<std::uint64_t>(v.tv_nsec) * 1'000'000'000);
    };
    return {Instant(static_cast<std::int64_t>(wall.tv_sec),
                    static_cast<std::uint64_t>(wall.tv_nsec) * 1'000'000'000),
            duration(resolution), duration(before), duration(after) - duration(before),
            "clock_gettime(CLOCK_REALTIME / CLOCK_MONOTONIC)"};
#endif
}
} // namespace iiCalendar
