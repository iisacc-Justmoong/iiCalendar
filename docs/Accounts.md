# Calendar accounts

iiCalendar 0.2 adds `CalendarAccountManager`, a native C++23 account layer over
the existing calendar providers. An authenticated, unexpired iisacc app session
is required before connecting, refreshing or reading an external calendar.
The iisacc account is the primary owner; Google, Microsoft and device-managed
Apple sources are additional connections owned by that identity.

## iisacc integration

Apps using iiAccountManager 0.2.9 or later can synchronize its verified session
without linking Qt into iiCalendar:

```cpp
#include <iiCalendar/IisaccAccount.h>
#include <iiAccountManager/AccountManager.h>

iiCalendar::CalendarAccountManager calendars;
iisacc::accounts::AccountManager login;
auto update = [&] { iiCalendar::synchronizeIisaccAccount(calendars, login); };
QObject::connect(&login, &iisacc::accounts::AccountManager::authenticatedChanged,
                 &login, update);
QObject::connect(&login, &iisacc::accounts::AccountManager::loginSessionChanged,
                 &login, update);
QObject::connect(&login, &iisacc::accounts::AccountManager::sessionEnding,
                 &login, [&] { calendars.setIisaccSession(std::nullopt); });
update();
// The account SDK owns its login UI, loginWithPassword()/restoreSession(),
// verified session cookies and secure storage.
```

Run the adapter on the account manager's thread. It copies native values, so
calendar network work can run on an application I/O worker. Keep the calendar
manager alive until its workers finish and disconnect observers before destroying
it. `IisaccAccount.h` is a header-only dependent-type adapter; including it or
linking the native calendar package does not discover/import Qt or iiAccountManager.
The real account-manager contract test is a separately configured consumer at
`examples/iisacc-bridge`. It uses the synthetic account fixture in `resources/`
and keeps the existing 30-second test limit. `IICALENDAR_TEST_IISACC_BRIDGE=ON`
in the SDK build now reports how to migrate instead of adding the peer SDK.

The adapter requires `isAuthenticated()`, a current matching `sessionBinding()`
and `authenticatedUntil()`. Display-only `readAccount()`/`readJson()` snapshots
do not authenticate Calendar. The effective expiry includes shorter credential
cookie lifetimes, rather than extending Calendar access to a longer server
session deadline. A native host with another verified iisacc authentication
implementation can supply the same `IisaccCalendarSession` through
`setIisaccSession`; this is a trusted authentication-subsystem input, not a JSON
login endpoint or token-signature verifier.

## External connections

Complete the provider's OAuth flow in the host and supply a current access-token
callback backed by its secure credential store. The SDK resolves the external
subject from the authenticated profile API, loads calendars, and returns an
account connection with a stable owner/provider/subject ID.

```cpp
auto google = calendars.connectGoogle([&] { return secureTokens.google(); });
auto outlook = calendars.connectMicrosoft([&] { return secureTokens.microsoft(); });
auto objects = calendars.calendars(google.id);
for (const auto &object : objects)
    calendars.loadEvents(google.id, object.id, requestedWindow);

calendars.setEnabled(outlook.id, false); // Clears its cached data immediately.
calendars.setEnabled(outlook.id, true);
calendars.refresh(outlook.id);
calendars.disconnect(google.id);
```

| Provider | Host authorization | SDK connection |
| --- | --- | --- |
| Google | OAuth `openid email https://www.googleapis.com/auth/calendar.readonly` | OIDC userinfo verifies the subject; Calendar v3 reads calendar lists and expanded events |
| Microsoft / Outlook | Delegated OAuth `User.Read Calendars.Read` | Graph `/me` verifies the subject; `/me/calendars` and `calendarView` read calendars/occurrences |
| Apple device sources | EventKit full-access authorization; host usage descriptions/entitlements | `connectApple(options)` connects device-managed iCloud/Google/Exchange/CalDAV/local sources; `options.sourceId` selects one source |
| Custom service | Host-authenticated external identity and provider | `connect(ExternalAccount, ExternalCalendarProvider)` reuses the same ownership/lifecycle rules |

Google and Microsoft acquire the current token for each request/page. Newly
supplied tokens are checked against the original subject before they reach
calendar endpoints; a different external account requires a separate connection.
Only a token fingerprint is cached for this check. Tokens are never included in
account descriptors, events, connection documents or SDK-generated errors.
There is no OAuth browser UI, credential issuance, refresh-token persistence or
automatic authorization prompt inside these synchronous provider methods.

Apple authorization is an explicit host action through
`AppleCalendarProvider::requestAccess`. The account source includes the verified
iisacc device identity, so an EventKit source from one device does not become a
different device's source merely by restoring a profile. Availability and OS
permissions remain platform-specific. See [ExternalCalendars.md](ExternalCalendars.md).

## Ownership and lifecycle

`accounts()` lists the required iisacc owner first, then external connections.
The primary connection cannot be disabled/disconnected through external-account
operations. Sign out through the iisacc account SDK; its observer retires the
calendar session. Unauthenticated/expired session reads expose no previous
account list. Network/data operations reject `AuthenticationRequired`.

Refreshing the same verified iisacc binding updates its profile/deadline while
preserving external connections. Replacing the subject, origin, user binding,
device or app-session ID retires all provider references and caches. Work already
running can finish at the transport level, but returns `StaleSession` and cannot
populate the replacement session. The same account's concurrent operations
return `Busy`. Disabling/disconnecting a connection invalidates its cached data;
late results cannot republish it, including after an immediate re-enable or a
replacement connection with the same external ID.

An external connection reports `Linked`, `Loading`, `Ready`, `Disabled`, `Failed`
or `AuthenticationRequired`. Temporary transport/rate-limit/invalid-snapshot
failures preserve an existing snapshot. Authentication or permission rejection
clears only the affected account. Imported event copies use the iisacc subject
as `ownerId`, retain external identity in properties, and keep namespaced event
and calendar IDs across providers and owners.

## Restoring connections

`connectionDocument()` emits `iiCalendar.AccountConnections`, schema 1, with
the iisacc origin/subject, external IDs, display names/emails and enabled flags.
`restoreConnections(document)` accepts only the current owner's complete, bounded
document and must run before providers are attached. Credentials, authentication
session IDs, event data and access tokens are excluded. Restored enabled accounts
report `AuthenticationRequired` until their providers/credentials are reattached;
disabled connections stay disabled when reattached.

An internal SHA-256 implementation preserves the credential fingerprint used
to avoid repeating a profile lookup for an unchanged token. Its iisacc MIT
counterpart license is installed with the package, and standard vectors are
checked by `iiCalendar.sha256`. Neither the bearer token nor the fingerprint is
written to the connection document.

The former `saveConnections(nativePath)` and `loadConnections(nativePath)` members
were removed. The separately configured `examples/file-provider` consumer uses
`connectionDocument()` and `restoreConnections()` with iiFileProvider for atomic
publication and bounded reads. The host chooses its private, owner-specific
application-state path and serializes writes to that path.
Session changes do not automatically choose or open another user's file.
Default limits are 32 external connections and 256 KiB per profile document.
Calendar/event/page limits retain the provider's independent bounds.

## Microsoft data contract

The provider uses delegated Microsoft Graph v1.0 and requests UTC times plus
immutable IDs. It expands events through `calendarView`, preserves remote IDs,
categories, organizer/attendee data, visibility, availability and reminders, and
filters the expanded result against the exact requested window. Windows time
zone IDs are mapped through ICU to the corresponding IANA zone. All-day dates
are recovered in the original zone, including 23/25-hour daylight-saving days.
HTML bodies use the plain `bodyPreview`; text bodies retain their content.

Pagination is bounded, repeated links/IDs and foreign-origin or foreign-resource
next links are rejected, and incomplete/invalid snapshots are never returned as
complete data. Cancelled events are excluded; authentication, permission and
rate-limit failures have explicit error codes. This connection API reads external calendars; existing `Store` remains
the explicit application-owned persistence interface.

Primary references:
[Google OIDC](https://developers.google.com/identity/openid-connect/openid-connect),
[Microsoft calendars](https://learn.microsoft.com/en-us/graph/api/user-list-calendars?view=graph-rest-1.0),
[Microsoft calendarView](https://learn.microsoft.com/en-us/graph/api/calendar-list-calendarview?view=graph-rest-1.0),
[Microsoft user profile](https://learn.microsoft.com/en-us/graph/api/user-get?view=graph-rest-1.0),
[Apple EventKit](https://developer.apple.com/documentation/eventkit/accessing-the-event-store).
