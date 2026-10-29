#pragma once
#include <iiCalendar/Accounts.h>
#include <type_traits>

namespace iiCalendar {
// Header-only integration with iiAccountManager >= 0.2.9. All account/Qt types
// are dependent: including this header never introduces a Qt dependency.
// Call on the source manager's thread after authenticated/login-session changes
// and clear on sessionEnding. The native calendar manager can serve I/O workers.
template<class IisaccAccountManager>
void synchronizeIisaccAccount(CalendarAccountManager &calendar, const IisaccAccountManager &source) {
    if (!source.isAuthenticated()) { calendar.setIisaccSession(std::nullopt); return; }
    const auto binding = source.sessionBinding();
    using Key = typename std::remove_cvref_t<decltype(binding)>::key_type;
    const auto text = [](const auto &map, const char *key) {
        return map.value(Key::fromLatin1(key)).toString().toStdString();
    };
    try {
        const auto milliseconds = source.authenticatedUntil().toMSecsSinceEpoch();
        auto seconds = milliseconds / 1000;
        auto fraction = milliseconds % 1000;
        if (fraction < 0) { --seconds; fraction += 1000; }
        IisaccCalendarSession session{text(binding, "origin"), text(binding, "subject"), text(binding, "userId"),
            text(binding, "sessionId"), text(binding, "deviceId"), source.account()->email().toStdString(),
            source.account()->displayName().toStdString(), Instant(seconds, static_cast<std::uint64_t>(fraction) * 1'000'000'000'000'000ULL)};
        if (!source.matchesSessionBinding(binding) || source.account()->sub().toStdString() != session.subject)
            throw ExternalCalendarError(ExternalCalendarErrorCode::AuthenticationRequired, "iisacc session changed");
        calendar.setIisaccSession(std::move(session));
    } catch (...) {
        calendar.setIisaccSession(std::nullopt);
        throw ExternalCalendarError(ExternalCalendarErrorCode::AuthenticationRequired, "a verified iisacc app session is required");
    }
}
} // namespace iiCalendar
