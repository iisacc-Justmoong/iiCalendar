# 외부 캘린더 객체 관리자

`CalendarObjectManager`는 외부 로그인 계정의 캘린더 목록과 일정 객체를 관리한다.
`GoogleCalendarProvider`와 `AppleCalendarProvider`가 실제 조회를 담당한다. 공개 API는
`include/iiCalendar/ExternalCalendar.h`에 있으며 Qt·LVRS·Society·iiAcountManager를 참조하지 않는다.

## 로그인 후 Google 캘린더 불러오기

호출 애플리케이션이 OAuth 로그인을 완료하고 `calendar.readonly` 범위의 access token을 얻은 뒤
관리자에 로그인 계정과 제공자를 등록한다. 토큰 콜백은 **같은 한 계정**의 현재 토큰을 반환해야 한다.
계정 ID는 관리자의 모든 제공자 사이에서 유일한 값으로 지정한다. 표시 이름이나 토큰을 ID로 쓰지 않는다.

```cpp
#include <iiCalendar/iiCalendar.h>

void loadGoogleCalendar(iiCalendar::CalendarObjectManager &manager,
                        iiCalendar::CalendarAccessToken currentToken) {
    auto provider = std::make_shared<iiCalendar::GoogleCalendarProvider>(currentToken);
    // login()은 바로 캘린더 목록을 조회한다. 애플리케이션 작업 스레드에서 호출한다.
    manager.login({"google:stable-account-subject", "google", "업무 계정"}, provider);

    const auto objects = manager.calendars("google:stable-account-subject");
    for (const auto &object : objects) {
        manager.loadEvents(object.accountId, object.id,
            {iiCalendar::Instant::parse("2026-10-01T00:00:00Z"),
             iiCalendar::Instant::parse("2026-11-01T00:00:00Z")});
    }
    const auto loaded = manager.calendars("google:stable-account-subject");
    // loaded의 각 CalendarObject.events를 UI나 Store에 전달한다.
}
```

토큰 발급·로그인 화면·갱신·Keychain 등 보안 저장은 호출 애플리케이션의 책임이다.
SDK는 토큰을 관리자 객체, Event, SQLite, 오류 메시지에 기록하지 않는다. 토큰 콜백은 매 요청/페이지마다
호출된다. 만료된 토큰을 자동 재로그인하거나 무한 재시도하지 않는다.

Google은 `calendarList.list`와 `events.list`를 호출하고 마지막 페이지까지 완료한 뒤 결과를 반환한다.
반복 일정은 `singleEvents=true`로 원격 회차를 펼치며, 취소·삭제된 회차를 제외한다.
일정의 제목·설명·URL·장소·시간대·종일 범위·생성/수정 시각·busy·공개 범위·참석자·첨부파일과
원격 ID·ETag·반복 원본 정보를 보존한다. Google 전용 모든 필드를 완전하게 보존하는 모델은 아니다.
Google이 조회 조건의 소수초를 무시하므로 요청 범위를 초 단위로 바깥쪽으로 넓히고 결과를 원래
정밀 범위로 다시 필터링한다. 종일 일정의 종료 날짜는 제외 경계이다.

기본 HTTPS는 libcurl 7.85+를 사용한다. 인증서·호스트를 검증하고 리다이렉트를 따라가지 않는다.
연결 제한은 10초, 개별 HTTP 요청 제한은 30초, 응답 제한은 16 MiB이다. 로그아웃은 진행 중인
HTTP 연결을 강제로 종료하지 않지만 그 결과가 관리자에 다시 반영되는 것을 차단한다.

`IICALENDAR_WITH_CURL=OFF`로 빌드하면 Google 제공자 생성 시 `CalendarHttpGet`을 주입한다.
iOS·tvOS·watchOS·visionOS에서는 기본 OFF이다. 주입 전송은 요청의 인증 헤더를 Google 외의 서버로
전달하지 않고 동일한 TLS·응답 상한·리다이렉트 정책을 지켜야 한다. 전송을 주입하지 않으면 첫 조회에
`Unsupported`를 반환한다. 커스텀 전송을 사용해도 Google 응답 변환과 페이지 처리는 SDK가 담당한다.

## Apple 캘린더 불러오기

macOS/iOS의 `AppleCalendarProvider`는 EventKit을 사용한다. 기기의 시스템 설정에 로그인하여 연결된
iCloud·Google·Exchange·CalDAV·로컬 캘린더를 읽는다. `Sign in with Apple`의 identity token을
캘린더 데이터 접근 토큰으로 사용하지 않는다. Apple 계정의 로그인과 원격 동기화는 운영체제가 관리한다.

```cpp
auto provider = std::make_shared<iiCalendar::AppleCalendarProvider>();
if (iiCalendar::AppleCalendarProvider::authorization() ==
    iiCalendar::AppleCalendarAuthorization::FullAccess) {
    manager.login({"apple:device-calendars", "apple", "기기 캘린더"}, provider);
    // 이후 manager.loadEvents(accountId, object.id, window)를 작업 스레드에서 호출한다.
}
```

읽기 접근이 아직 허용되지 않았다면 호스트가 `provider->requestAccess(completion)`를 호출하고,
성공 콜백에서 애플리케이션 작업 스레드로 이동하여 `login()`한다. 콜백은 OS 큐에서 실행될 수 있다.
콜백에서 사용하는 manager·애플리케이션 객체의 수명은 호스트가 보장해야 한다. SDK의 조회나
테스트가 자동으로 OS 권한 대화상자를 띄우지는 않는다.

- macOS 14+/iOS 17+: `NSCalendarsFullAccessUsageDescription`와 full access가 필요하다.
- 이전 OS 지원 시 `NSCalendarsUsageDescription`도 포함한다. 런타임 API는 버전에 따라 선택한다.
- sandboxed macOS 앱은 `com.apple.security.personal-information.calendars` entitlement가 필요하다.
- write-only 권한으로는 기존 캘린더를 읽을 수 없다.

`sources()`로 기기의 계정 소스 목록을 얻고 `AppleCalendarOptions.sourceId`를 지정하면 한 소스만
조회한다. 빈 sourceId는 모든 기기 소스를 조회한다. 없는 소스/캘린더를 다른 계정으로 대체하지 않는다.
EventKit이 이미 원격 반복을 펼친 결과를 사용하고, 원본 객체 ID와 회차 시각으로 회차 ID를 구성한다.
SDK가 EventKit의 원격 서버 동기화 완료 시점을 보장하지는 않는다. 명시적 `refresh()`/`loadEvents()`로
현재 OS 저장소를 다시 읽는다.

Apple 조회 범위는 한 번에 366일 이하이다. EventKit의 4년 초과 조회가 조용히 잘리는 동작을 피하기 위해
긴 범위를 오류로 반환한다. `NSDate`의 부동소수점 시각을 마이크로초 단위로 정규화하며 원본보다 높은
시간 정확도를 주장하지 않는다. EventKit의 종일/floating 날짜는 OS 기본 시간대로 해석하여 날짜가
밀리지 않도록 한다. Apple 제공자는 일정의 제목·메모·URL·장소·색상·시간대·종일 범위·
생성/수정 시각·busy·상태·원격 식별자를 변환한다. Apple 참석자·알림·첨부파일 전체 매핑은 포함하지 않는다.
macOS/iOS 이외의 플랫폼에서는 `available()==false`이며 조회 시 `Unsupported`를 반환한다.

## 객체 수명·조회·실패 계약

| API/객체 | 계약 |
|---|---|
| `login(account, provider)` | 계정을 등록하고 캘린더 목록을 즉시 조회한다. 같은 account.id의 재로그인은 기존 세션과 캐시를 교체한다. |
| `refresh(accountId)` | 전체 캘린더 목록을 교체한다. 남은 동일 캘린더의 일정 캐시는 보존하고 사라진 캘린더는 제거한다. 시간대/소스가 바뀌면 일정 캐시도 제거한다. |
| `loadEvents(accountId, object.id, window)` | 양수 길이의 시작 포함·종료 제외 기간을 조회한다. 성공 시 해당 캘린더의 일정과 loadedWindow를 **최신 조회 하나**로 교체한다. 여러 기간을 무제한 누적하지 않는다. |
| `calendars(accountId)` / `calendar(id)` | 참조 수명에 의존하지 않는 소유 복사본을 반환한다. 객체의 remote.id는 원격 ID, object.id는 관리자 ID이다. |
| `accounts()` | Loading/Ready/Failed/AuthenticationRequired 상태와 오류 코드를 반환한다. 실패를 빈 성공으로 처리하지 않는다. |
| `logout(accountId)` | 계정과 캘린더·일정 캐시 및 제공자에 대한 관리자 참조를 제거한다. 이미 반환된 복사본이나 호스트가 별도 저장한 객체는 호스트가 정리한다. |

관리자 ID는 제공자·계정·원격 캘린더·원격 일정 ID를 길이 접두사로 구분하여 만든다.
서로 다른 계정/캘린더의 동일 원격 ID가 Store의 전역 Event ID에서도 충돌하지 않는다.
Event의 calendarId는 관리자 캘린더 ID, ownerId는 로그인 계정 ID이다. 원격 ID는 `external.*` 속성에도
남는다. revision은 0으로 반환하고 실제 저장 시 Store가 관리한다.

모든 페이지·객체 검증이 성공한 뒤에만 해당 스냅샷을 교체한다. 전송 실패·잘못된 응답·상한 초과 때
이전 스냅샷은 보존하며 상태는 Failed이다. Google의 429 및 rate-limit 사유의 403은 RateLimited로
분류하고 캐시를 보존한다. 재시도 시점은 호스트가 정한다. 인증/읽기 권한이 거부되면 그 계정의 캐시를 제거하고
AuthenticationRequired 상태로 전환한다. 다른 계정에는 영향을 주지 않는다. 같은 계정의 중복 요청은
Busy 오류이다. 다른 계정은 별도 제공자를 통해 병행 조회할 수 있다. 로그아웃·재로그인 전의 완료는
StaleSession 오류이며 새 세션에 결과를 반영하지 않는다. manager/provider 파괴는 진행 중 호출이 끝난
후 수행해야 한다. Google 제공자 하나를 여러 계정에 공유하지 않는다.

기본 제한은 계정별 캘린더 1,000개·캘린더별 일정 10,000개이며 Google은 최대 100페이지이다.
모든 제공자는 완전한 스냅샷을 반환하고 반복 회차를 미리 펼쳐야 한다. 빈 ID·중복 ID·유효하지 않은
시간대·일정·반복 원본을 거부한다. 잘린 결과를 완전한 스냅샷으로 반환하지 않는다.
추가 외부 서비스는 `ExternalCalendarProvider` 인터페이스만 구현한다.

## Store 및 표시 모델에 연결

```cpp
const auto object = manager.calendar(calendarId).value();
for (const auto &event : object.events) store.put(event);
const auto details = iiCalendar::dayDetails(store, iiCalendar::Date(2026, 10, 2),
                                           iiCalendar::TimeZone("Asia/Seoul"));
```

관리자는 메모리 내 조회 객체를 관리하며 Store를 자동 수정하지 않는다. 위 코드는 명시적 upsert 예제이다.
재조회에서 사라진 원격 일정의 영구 저장소 삭제나 로그아웃 후 저장소 정리는 호출 애플리케이션의 책임이다.
loadedWindow 밖을 완전한 원격 일정 목록으로 간주하지 않는다. 원격 일정 편집/삭제, 웹훅, 증분 syncToken,
독립 iCloud CalDAV 로그인 클라이언트 및 OAuth 로그인 UI는 이 API에 포함하지 않는다.

## 1차 자료

- [Google calendarList.list](https://developers.google.com/workspace/calendar/api/v3/reference/calendarList/list)
- [Google events.list: 반복 확장·페이지·조회 시각](https://developers.google.com/workspace/calendar/api/v3/reference/events/list)
- [Google API 오류와 403/429 호출량 제한](https://developers.google.com/workspace/calendar/api/guides/errors)
- [Apple EventKit 저장소 접근](https://developer.apple.com/documentation/eventkit/accessing-the-event-store)
- [Apple 캘린더 접근 수준 및 구버전 API](https://developer.apple.com/documentation/eventkit/accessing-calendar-using-eventkit-and-eventkitui)
- [libcurl 리다이렉트 정책](https://curl.se/libcurl/c/CURLOPT_FOLLOWLOCATION.html)

Apple 조회 API·식별자·4년 범위 한계는 설치된 Apple SDK의 `EKEventStore.h`, `EKEvent.h`,
`EKCalendarItem.h`도 대조했다. JSON 의존성의 버전·공식 해시·라이선스는
`ThirdParty/nlohmann/README.md`에 기록했다.
