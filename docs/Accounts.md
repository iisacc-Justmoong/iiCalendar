# Calendar accounts

`CalendarAccountManager`은 호스트가 지정하는 작업 공간에서 Google, Microsoft,
Apple 및 사용자 제공 캘린더를 연결한다. `setWorkspace`에는 앱이 관리하는
안정적인 namespace, 소유자 식별자, 기기 식별자를 전달한다. 이 값은 연결 문서와
EventKit 소스의 범위를 구분하며 인증 증명이 아니다. 공급자의 인증과 자격 증명
보관은 호스트가 담당한다. 작업 공간을 닫거나 식별자가 바뀌면 기존 연결과
캐시를 폐기하고 진행 중 결과는 `StaleSession`으로 거절한다. 같은 식별자를
다시 설정하면 연결을 보존한다. 기본 생성 상태는 닫혀 있다.

기존 `setIisaccSession`은 검증된 세션 및 기본 계정을 사용하는 호환 경로이다.
기존 연결 식별자, schema 1 문서, 만료와 갱신 계약은 유지한다. 범용 작업 공간은
가상의 기본 계정을 만들지 않으며 이벤트에 iisacc 전용 속성을 쓰지 않는다.

## iisacc integration

Apps using iiAccountManager 0.2.9 or later can synchronize its verified session
without linking Qt into iiCalendarIntegration:

```cpp
#include <iiCalendarIntegration/IisaccAccount.h>
#include <iiAccountManager/AccountManager.h>

iiCalendarIntegration::CalendarAccountManager calendars;
iisacc::accounts::AccountManager login;
auto update = [&] { iiCalendarIntegration::synchronizeIisaccAccount(calendars, login); };
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
and keeps the existing 30-second test limit. `IICALENDARINTEGRATION_TEST_IISACC_BRIDGE=ON`
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
호스트는 `connect*`에 최초 조회 구간을 전달해 캘린더 목록과 해당 구간 일정을
한 번에 가져올 수 있다. Daymark는 이 경로에서 공급자 목록을 한 번만 조회한다.
기존 계정을 갱신하려면 `synchronize(accountId, window)`를 호출한다.

```cpp
auto google = calendars.connectGoogle([&] { return secureTokens.google(); }, {}, {}, requestedWindow);
auto outlook = calendars.connectMicrosoft([&] { return secureTokens.microsoft(); });
auto objects = calendars.calendars(google.id);

calendars.setEnabled(outlook.id, false); // Clears its cached data immediately.
calendars.setEnabled(outlook.id, true);
calendars.synchronize(outlook.id, requestedWindow);
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
`AppleCalendarProvider::requestAccess`. The account source includes the host workspace or verified iisacc device identity, so an EventKit source from one device does not become a
different device's source merely by restoring a profile. Availability and OS
permissions remain platform-specific. See [ExternalCalendars.md](ExternalCalendars.md).

## Ownership and lifecycle

`accounts()` lists connected providers. A generic workspace creates no primary
account. In the iisacc compatibility path it lists the owner first and that
primary cannot be disabled or disconnected here. Closing the workspace or
signing out retires its connections. Closed or expired contexts expose no
previous account list and network/data operations reject `AuthenticationRequired`.

```cpp
iiCalendarIntegration::CalendarAccountManager calendars;
calendars.setWorkspace(iiCalendarIntegration::CalendarWorkspaceIdentity{
    "org.example.calendar", workspaceId, deviceId
});
auto google = calendars.connectGoogle([&] { return secureTokens.google(); });
calendars.setWorkspace(std::nullopt);
// Wait for outstanding I/O before destroying the manager.
```

Refreshing the same verified iisacc binding updates its profile/deadline while
preserving external connections. Replacing the subject, origin, user binding,
device or app-session ID retires all provider references and caches. Transport
work already in progress may finish. The retired attachment stops further
calendar reads and returns `StaleSession`. The same account's concurrent operations
return `Busy`. Disabling/disconnecting a connection invalidates its cached data;
late results cannot republish it, including after an immediate re-enable or a
replacement connection with the same external ID.

An external connection reports `Linked`, `Loading`, `Ready`, `Disabled`, `Failed`
or `AuthenticationRequired`. Temporary transport/rate-limit/invalid-snapshot
failures preserve an existing snapshot. Authentication or permission rejection
clears only the affected account. Imported event copies use the workspace owner as `ownerId`, retain external
identity in properties, and keep namespaced event and calendar IDs across
providers and owners. The iisacc compatibility path also adds `iisacc.subject`.
계정 전체 동기화는 각 캘린더의 일정을 검증한 뒤 목록과 `loadedWindow`를 함께
교체한다. 어느 캘린더에서든 일시적 전송 오류나 호출 제한이 발생하면 직전
스냅샷을 보존한다. 권한 거부 시에는 기존과 같이 해당 계정의 데이터를 지운다.
새 스냅샷에는 기본 최대 100,000개 일정을 보관하며 `maximumSnapshotEvents`로
조절한다. 캘린더별 일정 상한도 적용된다. 같은 계정의 동시 요청은 `Busy`이며
연결을 폐기하면 진행 중인 요청의 결과를 게시하지 않는다.

## Restoring connections

`connectionDocument()` emits `iiCalendarIntegration.AccountConnections`, schema 1, with
the workspace namespace/owner or iisacc origin/subject, external IDs, display
names/emails and enabled flags. Generic origins use a `workspace:` prefix, a workspace scope marker, and distinct account IDs to keep legacy iisacc documents isolated even if configured origins overlap.
`restoreConnections(document)` accepts only the current owner's complete, bounded
document and must run before providers are attached. Credentials, authentication
session IDs, event data and access tokens are excluded. Restored enabled accounts
report `AuthenticationRequired` until their providers/credentials are reattached;
disabled connections stay disabled when reattached.
이전 `iiCalendar.AccountConnections`의 schema 1 문서도 읽을 수 있다. 모듈 이름을
변경해도 소유자와 연결 ID를 계산하는 형식은 같다.

An internal SHA-256 implementation preserves the credential fingerprint used
to avoid repeating a profile lookup for an unchanged token. Its iisacc MIT
counterpart license is installed with the package, and standard vectors are
checked by `iiCalendarIntegration.sha256`. Neither the bearer token nor the fingerprint is
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

`workspaceOpen()` reports whether either workspace type is active. `authenticated()` retains its original meaning: it reports an unexpired iisacc session, and remains false for a generic workspace. External provider authentication is independent of both checks.

## Python account API contract

The optional `iicalendarintegration` native module exposes `CalendarAccountManager` and
host-owned `CalendarWorkspaceIdentity`, the Google/Microsoft/Apple connectors,
custom callback providers, account and calendar snapshots, event loading,
enabling, disconnecting and credential-free connection document restoration.
Token and HTTP callbacks remain host-owned. Calls perform synchronous I/O and
must not outlive the manager or Python interpreter. Calendar errors carry an
`ExternalCalendarErrorCode`; workspace identities are not authentication.


### Build and use

Install Python development headers and pybind11 2.12 or later in the build
interpreter, then configure the optional binding and run its tests:

```sh
cmake -S . -B build -DIICALENDARINTEGRATION_BUILD_PYTHON=ON \
  -Dpybind11_DIR="$(python3 -m pybind11 --cmakedir)"
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build
```

The extension is installed under `<prefix>/lib/pythonX.Y/site-packages` by
default. `IICALENDARINTEGRATION_PYTHON_INSTALL_DIR` changes that prefix-relative path. Add
it to the matching Python interpreter's module search path. On Windows, also
make the installed `bin` directory available to the DLL loader, for example
with `os.add_dll_directory`. The Python and native binaries must target the
same architecture. CMake defaults the binding to OFF, so native-only consumers
need neither Python development headers nor pybind11. The pybind11 BSD license
is included in Python-enabled installations. This binding covers the
account manager and its calendar/event snapshots; it is not a binding for the
entire iiCalendarIntegration chronology and storage APIs.

```python
import iicalendarintegration as cal

manager = cal.CalendarAccountManager()
manager.set_workspace(cal.CalendarWorkspaceIdentity("org.example.app", owner_id, device_id))
# secure_store must return a current OAuth access token for this account.
account = manager.connect_google(secure_store.google_access_token, initial_window=requested_window)
for calendar in manager.calendars(account.id):
    print(calendar.events)
manager.synchronize(account.id, requested_window)
document = manager.connection_document()  # no access tokens
manager.set_workspace(None)
```

Use `connect_microsoft` similarly; `connect_apple(AppleCalendarOptions())`
uses device-managed EventKit sources. `AppleCalendarProvider.available()`,
`authorization()`, `sources()` and asynchronous `request_access(callback)`
expose the native authorization workflow. The host supplies platform usage
descriptions/entitlements and must keep Python and the provider alive until the
callback completes. A callback exception is reported as an unraisable Python
exception. Apple authorization and available sources depend on the OS.

`connect_google` and `connect_microsoft` accept optional Python `get(request)`
callbacks returning `CalendarHttpResponse(status, body)` for custom transport;
without one they use the SDK transport. `connect_custom` accepts an
`ExternalAccount`, a calendar-list callback and an event-list callback. The
manager retains callbacks while connected. Calls release the GIL around native
work and reacquire it for callbacks; callback implementations must be
thread-safe and must not recursively operate on the same connection. Wait for
workers and Apple callbacks before shutting down the interpreter. Python
subinterpreters are not supported. `CalendarError.code` identifies native
errors; callback exceptions are translated to sanitized native errors.
`Event`, calendar and account lists are snapshots. Python lists/maps are copies:
assign changed lists or maps back to their attribute. `TimeRange` uses half-open
RFC 3339 instants; all-day schedules use exclusive end dates.
