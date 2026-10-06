# iiCalendar

C++23-based Qt non-dependent calendar SDK. It provides high-precision time, international calendar and timezone, schedule, to-do, activity, SQLite index, and data models for screen display. It does not reference the UI framework.

The public API is located at `include/iiCalendar/` and the implementation is split into existing `src/Date`, `Time`, `Calendar`, `Event`, and `Index`. Usage examples are `examples/day_details.cpp`.

```sh
python3 scripts/bootstrap_icu.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 6
ctest --test-dir build --output-on-failure
./build/iiCalendarExample
cmake --install build --prefix "$PWD/build/install"
```

If ICU already exists, specify `-DICU_ROOT=/path/to/icu` without bootstrap. The required dependencies are SQLite 3.37+, ICU 69+, and C++23 compiler. Bootstrap validates official ICU 78.3 sources from POSIX host to SHA-256 and statically builds only at `build/deps/icu/`. Network download is performed only during explicit bootstrap execution. CMake is not downloaded.

```cmake
find_package(iiCalendar 0.1 CONFIG REQUIRED)
target_link_libraries(MyApplication PRIVATE iiCalendar::iiCalendar)
```

Installation consumers require SQLite and ICU development package or `ICU_ROOT`. The top-level SDK installer is auto-discovered via CMakeLists.txt. To update the actual user installation on the host, use the SDK installer's `--only iiCalendar` and the desired prefix.

The specific supported scope, time scales, recurrence rules, and storage contracts are recorded in [design documentation](docs/Design.md); the scope of tests and platform verification is recorded in [verification documentation](docs/Validation.md).

## 외부 캘린더 객체 관리자

`CalendarObjectManager`에 로그인 계정과 `GoogleCalendarProvider` 또는 `AppleCalendarProvider`를
등록하면 해당 계정의 캘린더 목록을 불러온다. `loadEvents()`는 지정 기간의 일정을 기존 `Event`
객체로 제공하며 `refresh()`·`logout()`으로 갱신과 계정별 객체 수명을 관리한다. Qt 의존성은 없다.

Google은 호스트가 발급한 OAuth 토큰 콜백과 Calendar API를 사용한다. 기본 전송은 libcurl 7.85+이며
`-DIICALENDAR_WITH_CURL=OFF`에서는 호스트의 `CalendarHttpGet`을 주입할 수 있다. Apple은 macOS/iOS
EventKit으로 기기에 연결된 계정의 캘린더를 읽으며 OS full-access 권한이 필요하다.
로그인 연결 예제, 조회·실패·캐시 계약과 지원 범위는 [외부 캘린더 문서](docs/ExternalCalendars.md)에 기록했다.
