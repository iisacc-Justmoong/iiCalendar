# iiCalendar 0.1 설계 및 지원 계약

## 의존 방향

`Date / Time → Calendar / TimeZone → Event / Recurrence → Store → View / DayDetails`

화살표는 상위 기능이 앞선 하위 기능을 사용한다는 뜻이다. Society, LVRS, Qt 및 다른 상위 SDK를
참조하지 않는다. 공개 헤더에 ICU·SQLite 자료형을 노출하지 않는다. `CalendarSystem`은 사용자 정의
역법의 교체 경계이고, `Store::Impl`은 연결·스키마를 숨긴다. 레지스트리 등록은 공유·사용 전에
완료해야 한다. 같은 Store 객체의 접근은 mutex로 직렬화된다.

## 날짜·시각·정밀도

| 객체 | 의미 |
|---|---|
|Date| 선행 그레고리력, 천문학적 연도 -999999..999999, 연도 0 포함 |
| CalendarDate | 역법의 확장 연도·월·일·윤월·시대·시대 내 연도 |
| WeekDate | ISO 주차 연도·주차·요일, 월요일 1부터 일요일 7 |
|Time| 하루 내 초 0..86399 및 소수 18자리 |
|Instant| Unix epoch 기준 signed 64-bit 초 + 소수부 0..10¹⁸−1 |
|지속 시간| 동일한 정수 쌍으로 표현한 경과 시간 |
| LocalDateTime | 시간대가 결합되지 않은 지역 날짜·시각 |
| ZonedDateTime | 지역 날짜·시각, UTC 오프셋, DST 여부, 시간대 ID |
| TimeRange | 시작 포함·종료 제외. 양끝이 같으면 단일 시점 이벤트 |

`Instant(-1, 999999999999999999)`는 epoch 직전 1 attosecond이다. 정밀 연산과 저장에
부동소수점을 사용하지 않는다. 정규화 후 표현 범위를 넘으면 `std::overflow_error`를 발생시킨다.
ISO 문자열은 필요한 소수부만 최대 18자리까지 표시한다. 초과 자릿수, 오프셋 없는 절대 시각,
알 수 없는 오프셋 `-00:00`은 반올림·추정 없이 거부한다.
직렬화 형식은 프로세스 전역 로케일의 숫자 그룹 구분자에 영향을 받지 않는다.

**표현 정밀도와 물리 시계 정확도는 다르다.** `Clock::sample()`은 macOS/Linux의
`clock_gettime`·`clock_getres`, Windows의 `GetSystemTimePreciseAsFileTime`·QPC를 사용한다.
벽시계·단조 시계·관측 구간·출처를 반환한다. `wallResolution`은 OS가 제공한 분해능이며,
Windows에서는 알 수 없음(`nullopt`)이다. FILETIME의 100 ns 표현 단위를 UTC 정확도로 주장하지
않는다. OS 시계 설정을 변경하지 않는다.

시각 척도는 POSIX/Unix 시간이다. 윤초 `23:59:60`을 거부한다. TAI/TT/UT1/GPS 변환,
NTP 동기화 정확도 보장은 포함하지 않는다. 그레고리·율리우스력은 선행 역법이며 국가별 역사적
개력 날짜를 자동 적용하지 않는다.

## 역법·시간대

자체 정수 구현은 `gregorian`, `iso8601`, `julian`이다. ISO 주차는 `WeekDate`로 제공한다.
국제 역법은 ICU의 실제 등록 목록에서 발견한다. 이번 ICU 78.3 빌드의 19종은 다음과 같다.

```text
buddhist, chinese, coptic, dangi, ethiopic, ethiopic-amete-alem,
gregorian, hebrew, indian, islamic, islamic-civil, islamic-rgsa,
islamic-tbla, islamic-umalqura, iso8601, japanese, julian, persian, roc
```

`CalendarRegistry::ids()`가 현재 지원 목록을 반환한다. 한국 음력은 `dangi` 어댑터이며 윤월을
별도 필드로 보존한다. 역사적·관측적 변형까지 모두 같은 달력이라고 간주하지 않는다. 추가 역법은
`CalendarSystem`을 구현해 `add()`로 등록한다. 알 수 없는 역법을 그레고리력으로 대체하지 않는다.

ICU에는 완전한 초를 정확한 정수 밀리초로 전달하고, 소수 18자리는 별도 보존한 뒤 다시 결합한다.
ICU 역법·시간대 변환 한계는 epoch 기준 ±100,000,000일이다. 핵심 Instant 범위와 Date·문자열·
ICU 변환 범위는 다르며 각 경계를 검사한다. 날짜는 지역 자정 기준이다. 일몰 시작 종교일의 관측
시각을 자동 계산하지 않는다.

`TimeZone::system()`은 현재 호스트 시간대, `available()`은 지원 목록, `databaseVersion()`은
tzdb 버전을 반환한다. 잘못된 시간대 ID를 거부한다.

- DST 중복 시각: `Reject / Earlier / Later` 중 선택한다.
- 존재하지 않는 지역 시각: `Reject / ShiftForward` 중 선택한다.
- `dayRange()`는 지역 자정 경계를 계산한다. 하루는 23/25시간이거나 생략된 날의 경우 길이 0이다.
- `AllDaySchedule`은 끝 날짜를 제외하는 날짜 범위이다. 하루를 고정 86400초로 취급하지 않는다.

## 이벤트·반복

EventKind는 일정·할 일·활동·메모·알림이다. 컬렉션 ID, 역법 ID, 제목·설명·URL·색상·소유자,
공개 범위, busy, 우선순위, 완료율, 생성·수정·마감·완료 시각, 장소·좌표, 참석자·응답,
파일, 알림, 태그, 확장 속성, 관계 링크를 포함한다. `calendarId`는 컬렉션이고 `calendarSystem`은 역법이다.

일정은 시간 구간·종일 날짜 구간·미지정 중 하나이다. 미지정 이벤트에 마감이 있으면 마감 시점에
배치한다. 시간 일정과 마감이 모두 있으면 달력 배치는 시간 일정 기준이다. 둘 다 없는 할 일은
저장·필터 조회할 수 있지만 특정 날짜에 자동 배치하지 않는다.

반복은 일·주·월·연, 간격, 횟수 또는 종료 시각, 요일·월·월의 일자, 월의 n번째/마지막 요일,
제외·추가 시점을 지원한다. 음수 일자는 월말 기준이다. 월 주기는 역법의 실제 윤월·13번째 월
경계를 따라 계산하며 연 주기는 확장 연도를 사용한다. 시작일은 규칙의 기준점이며 필터에도
일치해야 회차가 생성된다. 이 모델은 명시된 규칙 부분집합이며 RFC 5545 전체 구현은 아니다.
초·분·시간 단위 반복, BYSETPOS, 개별 회차 수정 객체, ICS 파서·독립 CalDAV 클라이언트는 포함하지 않는다.
외부 계정의 조회는 별도 `CalendarObjectManager`·제공자를 사용한다. Google/EventKit에서 펼친 회차를
기존 Event로 변환하며 로컬 반복 규칙으로 다시 펼치지 않는다.

지역 시각을 유지하며 timed 이벤트는 경과 길이, 종일 이벤트는 날짜 수를 보존한다. DST 공백 회차는
건너뛰고 중복 시각은 앞 시각을 사용한다. 최초 시점이 뒤 시각으로 지정됐다면 최초 시점은 유지한다.
제외 회차도 규칙 횟수에 포함한다. 추가 회차는 횟수·종료 조건 밖에서도 추가 가능하다. 같은 시점은
중복 제거하며 제외가 우선한다. 종료 시각은 포함한다.

ExpansionLimits 기본값은 반환 10,000건·날짜 스캔 1,000,000회이다. 초과하면 조용히 자르지 않고
`std::length_error`를 발생시킨다. 현재 시작점부터 날짜를 순회하므로 매우 오래된 대규모 반복은
조회 범위나 제한을 조정해야 한다.

## SQLite

STRICT 테이블, WAL, FULL 동기화, foreign key, 5초 busy timeout을 사용한다. application_id와
user_version=1을 확인하며 타 애플리케이션 DB·미지원 스키마를 거부한다. 부모 디렉터리를 자동
생성하거나 기존 DB를 삭제하지 않는다. `:memory:`를 지원한다.

events에는 절대 시작·종료의 정수 `(seconds, attoseconds)`, 지역 날짜 epoch day,
하루 내 초·소수부, 시간대, 종일 여부, 컬렉션·유형·상태·제목·revision을 함께 저장한다.
전체 객체는 버전이 있는 little-endian payload로 보존한다. payload 최대 16 MiB,
각 텍스트 1 MiB, 개별 컬렉션 10,000개이다. 읽을 때 길이·타입·객체 불변조건과 ID/revision을 검사한다.
쓰기 도중에도 누적 payload 상한을 검사해 과대 입력을 끝까지 할당하지 않는다.

| 인덱스 | 목적 |
|---|---|
|idx_events_start / end| 정밀 시간 구간 |
| idx_events_calendar_start | 컬렉션별 시간순 |
| idx_events_date_time / calendar_date | 저장된 지역 시작 날짜·시각 |
| idx_events_kind_status | 유형·진행 상태 |
| idx_events_due | 마감 시각 |
| idx_events_recurring | 반복 후보 |
| idx_tags_lookup | 태그 → 이벤트 |

SQLite REAL·julianday()·날짜 함수로 소수초를 변환하지 않는다. 정수 쌍의 사전식 비교를 사용한다.
put()은 payload·시간 인덱스·태그를 BEGIN IMMEDIATE 트랜잭션에서 갱신한다. 실패하면 전체
롤백한다. expectedRevision=0은 신규 생성 전용이고, revision이 다르면 RevisionConflict를 반환한다.

- query(): 원본 이벤트 조회. 반복 회차를 펼치지 않으며 취소 이벤트도 조회 가능하다.
- onLocalDate(): 각 이벤트 시간대에서 **저장된 시작 날짜**를 조회한다. 날짜를 가로지르는 일정,
  다른 시간대, 반복 회차의 겹침은 scheduled() 또는 dayDetails()를 사용한다.
- scheduled(): 하나의 SQLite 읽기 스냅샷에서 후보 객체를 얻어 반복을 확장한다. 취소 항목을 제외한다.
- query() 페이지는 최대 10,000건이다. onLocalDate()와 scheduled() 후보도 10,000건으로
  제한하며 초과 시 실패한다. 부분 집계를 완전한 결과로 반환하지 않는다.

## 표시 모델

ViewKind는 일·주·월·연·agenda·timeline·사용자 지정 기간을 지원한다. 월은 42셀, 다른 뷰는
실제 기간의 날짜 배열을 제공한다. 주 시작 요일·오늘·역법을 인자로 받고 한 셀에 그레고리 날짜와
선택 역법의 날짜를 함께 제공한다. UI 위젯을 그리는 라이브러리는 아니다.

dayDetails()는 선택 날짜·시간대의 일정·할 일·활동·메모·알림을 집계한다. 각 영역은 전체 건수,
지정 길이의 미리보기, hasMore()를 반환한다. 완료된 할 일 수, 중복 제거된 파일 목록·건수,
hasMoreFiles()도 제공한다. 동일 컬렉션의 동일 첨부 ID가 중복 제거 기준이다. 여러 날에 걸친
일정은 겹치는 각 날에 나타난다. Figma의 View all … 버튼에 이 상태를 연결할 수 있다.

## 1차 자료·의존성 고지

외부 캘린더 의존 방향은 `ExternalCalendarProvider → CalendarObjectManager → 호출 애플리케이션`이다.
제공자와 관리자는 기존 Event/TimeZone을 사용하고 상위 제품·로그인 프레임워크를 참조하지 않는다.
Google의 비공개 JSON 변환과 libcurl 전송, Apple의 비공개 Objective-C++ EventKit 구현은 공개 C++23
인터페이스 뒤에 둔다. 계정·캘린더·일정 ID, 부분 실패·로그아웃 계약은
[외부 캘린더 문서](ExternalCalendars.md)에 정의한다.

- [ICU 캘린더 서비스](https://unicode-org.github.io/icu/userguide/datetime/calendar/)
- [ICU 78.3, tzdb 2026a 배포](https://github.com/unicode-org/icu/releases/tag/release-78.3)
- [SQLite 데이터 형식](https://www.sqlite.org/datatype3.html)
- [SQLite 정수 복합 값 비교](https://www.sqlite.org/rowvalue.html)
- [Windows 고정밀 시계](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getsystemtimepreciseasfiletime)

ICU의 Unicode 라이선스 원문은 bootstrap 소스의 build/deps/icu/LICENSE에 포함된다.
ICU 재배포 시 관련 고지를 유지해야 한다. SQLite는 공급 배포본의 고지를 확인한다.
