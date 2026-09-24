#pragma once
#include "Event.h"
#include <filesystem>
#include <mutex>
#include <stdexcept>

namespace iiCalendar {
class StorageError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
class RevisionConflict : public StorageError {
    using StorageError::StorageError;
};
struct EventQuery {
    std::optional<TimeRange> window; // Stored/base times, not expanded recurrence.
    std::optional<std::string> calendarId, tag;
    std::optional<EventKind> kind;
    std::optional<EventStatus> status;
    std::size_t limit = 100;
    std::size_t offset = 0;
};
struct ScheduledEvent {
    Event event;
    Occurrence occurrence;
};
class Store {
  public:
    explicit Store(
        const std::filesystem::path &database,
        std::shared_ptr<const CalendarRegistry> calendars = std::make_shared<CalendarRegistry>());
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;
    // expectedRevision=0 means create-only. No value means unconditional upsert.
    Event put(Event event, std::optional<std::uint64_t> expectedRevision = std::nullopt);
    std::optional<Event> get(const std::string &id) const;
    bool erase(const std::string &id, std::optional<std::uint64_t> expectedRevision = std::nullopt);
    std::vector<Event> query(const EventQuery &query = {}) const;
    std::vector<Occurrence> occurrences(TimeRange window,
                                        const std::optional<std::string> &calendarId = {},
                                        ExpansionLimits limits = {}) const;
    std::vector<ScheduledEvent> scheduled(TimeRange window,
                                          const std::optional<std::string> &calendarId = {},
                                          ExpansionLimits limits = {}) const;
    // Exact indexed fields are materialized atomically with the event payload.
    std::vector<Event> onLocalDate(Date date,
                                   const std::optional<std::string> &calendarId = {}) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace iiCalendar
