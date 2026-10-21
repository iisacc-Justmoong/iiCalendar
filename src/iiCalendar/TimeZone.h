#pragma once
#include "Time.h"
#include <vector>

namespace iiCalendar {
enum class AmbiguousTime { Reject, Earlier, Later };
enum class MissingTime { Reject, ShiftForward };
struct ZonedDateTime {
    LocalDateTime local;
    int utcOffsetSeconds;
    bool daylightSaving;
    std::string zoneId;
};
class TimeZone {
  public:
    explicit TimeZone(std::string id = "UTC");
    static TimeZone system();
    static std::vector<std::string> available();
    static std::string databaseVersion();
    const std::string &id() const noexcept { return id_; }
    ZonedDateTime at(Instant instant) const;
    Instant resolve(LocalDateTime local, AmbiguousTime ambiguous = AmbiguousTime::Reject,
                    MissingTime missing = MissingTime::Reject) const;
    TimeRange dayRange(Date date) const;

  private:
    std::string id_;
};
} // namespace iiCalendar
