# iiCalendar

C++23 기반의 Qt 비의존 캘린더 SDK이다. 고정밀 시간, 국제 역법·시간대, 일정·할 일·활동,
SQLite 인덱스와 화면 표시용 데이터 모델을 제공한다. UI 프레임워크를 참조하지 않는다.

공개 API는 `include/iiCalendar/`에 있으며 구현은 기존 `src/Date`, `Time`, `Calendar`,
`Event`, `Index`에 분리되어 있다. 사용 예제는 `examples/day_details.cpp`이다.

```sh
python3 scripts/bootstrap_icu.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 6
ctest --test-dir build --output-on-failure
./build/iiCalendarExample
cmake --install build --prefix "$PWD/build/install"
```

ICU가 이미 있다면 bootstrap 없이 `-DICU_ROOT=/path/to/icu`를 지정한다.
필수 의존성은 SQLite 3.37+, ICU 69+, C++23 컴파일러이다. bootstrap은 POSIX 호스트에서
공식 ICU 78.3 소스를 SHA-256으로 검증하고 `build/deps/icu/`에만 정적으로 빌드한다.
네트워크 다운로드는 명시적 bootstrap 실행에서만 수행한다. CMake는 다운로드하지 않는다.

```cmake
find_package(iiCalendar 0.1 CONFIG REQUIRED)
target_link_libraries(MyApplication PRIVATE iiCalendar::iiCalendar)
```

설치 소비자는 SQLite와 ICU 개발 패키지 또는 `ICU_ROOT`가 필요하다.
상위 SDK 설치기는 CMakeLists.txt를 통해 자동 발견한다. 호스트의 실제 사용자 설치본을
갱신하려면 SDK 설치기의 `--only iiCalendar`와 원하는 prefix를 사용한다.

구체적인 지원 범위, 시간 척도, 반복 규칙, 저장 계약은 [설계 문서](docs/Design.md),
테스트와 플랫폼 검증 범위는 [검증 문서](docs/Validation.md)에 기록한다.
