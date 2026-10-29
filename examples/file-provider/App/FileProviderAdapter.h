#pragma once
#include <iiCalendar/iiCalendar.h>
#include <iiFileProviderNative.h>
#include <iiFileProviderStorage.h>
#include <sqlite3.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

// Application-owned composition: neither SDK imports the other SDK.
namespace iiCalendarFileProvider {
inline std::shared_ptr<sqlite3> openDatabase(const std::filesystem::path &path) {
    const auto utf8 = path.u8string();
    const std::string file(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    if (file.empty() || file.find('\0') != std::string::npos)
        throw std::invalid_argument("database path is required");
    sqlite3 *raw = nullptr;
    const int opened = iiFileProvider::native::openDatabase(file.c_str(), &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> owned(raw, sqlite3_close_v2);
    if (opened != SQLITE_OK)
        throw iiCalendar::StorageError(raw ? sqlite3_errmsg(raw) : "cannot allocate SQLite connection");
    return std::shared_ptr<sqlite3>(std::move(owned));
}
inline iiCalendar::Store openStore(const std::filesystem::path &path,
    std::shared_ptr<const iiCalendar::CalendarRegistry> calendars = std::make_shared<iiCalendar::CalendarRegistry>()) {
    if (!calendars) throw std::invalid_argument("calendar registry is required");
    return iiCalendar::Store(openDatabase(path), std::move(calendars));
}
inline void saveConnections(const iiCalendar::CalendarAccountManager &accounts,
                            const std::filesystem::path &path) {
    iiFileProvider::native::File::write(path, accounts.connectionDocument());
}
inline void loadConnections(iiCalendar::CalendarAccountManager &accounts,
                            const std::filesystem::path &path) {
    if (!accounts.authenticated())
        throw iiCalendar::ExternalCalendarError(iiCalendar::ExternalCalendarErrorCode::AuthenticationRequired,
                                                 "an authenticated iisacc session is required");
    accounts.restoreConnections(iiFileProvider::native::File::read(
        path, iiCalendar::CalendarAccountManager::maximumConnectionBytes));
}
} // namespace iiCalendarFileProvider
