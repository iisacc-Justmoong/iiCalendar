# 검증

## iisacc 필수 계정과 외부 계정 연결 (2026-10-16, 0.2.0)

macOS arm64 Debug에서 Qt 검색을 비활성화한 네이티브 빌드와 libcurl OFF 빌드를
검증했다. 각 구성은 계정·Microsoft Graph·기존 도메인·외부 제공자·설치 소비자를
포함한 CTest 15/15이다. 계정 계약 검사 52개와 Microsoft 제공자 검사 29개가
포함된다. 설치 소비자는 Qt6와 iiAcountManager 검색을 명시적으로 금지하고,
설치된 공개 헤더·정적 라이브러리·CMake export만으로 새 계정 API를 빌드·실행한다.

| 검증 | 범위 |
| --- | --- |
| accounts | iisacc 인증 필수, 기본 계정 보호, 선택적 userId, 소유자·제공자별 ID 격리, 동일 세션 갱신, 계정 전환·만료·로그아웃 정리, 연결 비활성화·복원, 다른 소유자의 문서 거부 |
| 계정 자격 | Google OIDC·Microsoft Graph 프로필 조회, 새 토큰의 원래 외부 계정 바인딩, 다른 계정 토큰 거부, rate-limit 시 기존 데이터 보존, 인증 오류 시 해당 계정만 정리 |
| 늦은 응답 | 소유자 교체, 같은 외부 ID의 연결 교체, 제공자 시작 중 비활성화·즉시 재활성화 뒤 이전 요청의 데이터 재게시 차단 |
| 연결 저장 | iiFileProvider를 통한 파일 저장·읽기, iisacc origin/subject 귀속, 토큰·앱 세션 ID 제외, 복원 후 제공자 재연결 필요 |
| external_microsoft | 목록 페이지·헤더·ID 인코딩, 반복 회차·소수초·시간대·종일 일정·DST 25시간·알림 변환, 취소 제외, HTTP 오류 분류, 중복·상한·반복/외부 페이지·헤더 주입 거부 |
| iisacc_bridge | 실제 iiAcountManager 0.2.9와 합성 loopback 로그인 서비스, 표시용 프로필 비인증, 검증된 세션·실효 만료 전달, 외부 연결 소유자 바인딩, clear() 후 정리 |

인증 브리지 검사는 Qt가 있는 별도 호스트 구성 `build/accounts-bridge`에서 1/1을
실행한다. 네이티브 라이브러리의 Qt 의존성을 추가하는 구성이 아니다.
iiAcountManager 자체의 모델·응답 계약 2/2와 네트워크의 새 실효 만료·기존 로컬 만료
회귀 검사도 통과했다. 서버 세션보다 짧은 쿠키 만료를 네이티브 소비자에게 전달하며,
미인증·만료 상태는 유효하지 않은 `QDateTime`을 반환한다.

```sh
cmake -S . -B build/accounts -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON -DCMAKE_PREFIX_PATH=/path/to/iiFileProvider
cmake --build build/accounts --parallel 6
ctest --test-dir build/accounts --output-on-failure
cmake -S . -B build/accounts-no-curl -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON -DIICALENDAR_WITH_CURL=OFF \
  -DCMAKE_PREFIX_PATH=/path/to/iiFileProvider
cmake --build build/accounts-no-curl --parallel 6
ctest --test-dir build/accounts-no-curl --output-on-failure
# Optional real account-SDK integration; Qt belongs to this host/test only.
cmake -S . -B build/accounts-bridge -DIICALENDAR_TEST_IISACC_BRIDGE=ON \
  '-DCMAKE_PREFIX_PATH=/path/to/iiFileProvider;/path/to/iiAcountManager;/path/to/Qt'
cmake --build build/accounts-bridge --target iiCalendarIisaccBridgeTests --parallel 6
ctest --test-dir build/accounts-bridge -R '^iiCalendar.iisacc_bridge$' --output-on-failure
```

구현 전 계정 계약 실패는 `build/accounts-tdd-red.log`, 즉시 재활성화 경합의 회귀 실패는
`build/accounts-toggle-tdd-red.log`이다. 최종 빌드·검사 로그는 `build/accounts-build.log`,
`build/accounts-tests.log`, `build/accounts-no-curl-*.log`, `build/accounts-bridge-*.log`이며,
각 빌드 디렉터리의 JUnit XML에도 결과를 보존한다. 계정 SDK의 근거는 해당 모듈의
`build/calendar-bridge-build.log`, `build/calendar-bridge-model-tests.xml`,
`build/calendar-bridge-network-tests.log`이다. SDK 설치 산출물은 Workspace의 `build/`
아래에 두었다.

`build/accounts-ownership.json`의 SDK/Product 파일 생성 경로 감사는 런타임 소스
2,470개에서 우회 생성 0건·읽기 실패 0건이다. 새 연결 프로필도 iiFileProvider를
거쳐 생성한다.

외부 서비스 응답과 iisacc 로그인은 합성 fixture이다. 실제 Google/Microsoft OAuth,
운영 iisacc 로그인, Apple 캘린더 권한 승인·실계정 일정 읽기는 수행하지 않았다.
Apple의 실제 권한 상태 API·네이티브 변환 검사는 기존처럼 포함하지만 권한을 요청하지
않는다. Windows/Linux/Android/iOS/WASM 빌드·실행 결과를 의미하지 않는다.
외부 계정 인증 UI·토큰 발급·보안 저장소는 호스트가 소유한다.

## 외부 캘린더 객체 관리자 (2026-10-02)

macOS arm64 Release 빌드에 관리자·Google·Apple EventKit 제공자와 네이티브 변환 검사를 추가했다.
CTest 13/13이 통과했다. 기존 8개 도메인·외부 4개 묶음·설치 소비자 1개로 구성되며
최종 컴파일 경고는 없었다. 아래 로그에 실제 결과를 기록한다.

| 묶음 | 검증 |
|---|---|
| external_manager | 로그인 즉시 목록 조회, 계정 간 동일 원격 ID 격리, Event/Store/dayDetails 연결, 기간 캐시 교체, 실패 보존, 인증 거부 정리, 캘린더 삭제 반영, 상한, 중복/반복 원본 거부, Busy, 로그아웃/재로그인 중 늦은 완료 차단 |
| external_google | 다중 페이지와 페이지 토큰 인코딩, 시간/종일·시간대·참석자·반복 회차 메타데이터 변환, 취소 제외, 401/403/429 분류, 반복 페이지·페이지/객체/응답 상한, 토큰 부재/헤더 주입 거부, 혼합 날짜 거부, 소수초 기간 확대와 정확한 필터링, 지역 시각 입력 |
| external_apple | 실제 EventKit 권한 상태 API 호출, 읽기 권한을 요청하지 않고 366일 초과 범위 거부 |
| external_apple_codec | OS 계정을 읽지 않는 Objective-C 선택자 fixture와 실제 NSDate/NSTimeZone으로 메타데이터·회차 ID·음수 epoch 마이크로초·종일 날짜의 OS 기본 시간대·뉴욕 DST 25시간·취소 상태·누락 값 변환 확인 |
| installed_consumer | build/install의 공개 헤더·정적 라이브러리·CMake export만 사용해 사용자 제공자·관리자·Google 제공자·Apple 네이티브/대체 구현을 별도 프로젝트에서 빌드/실행 |

```sh
cmake -S . -B build -DIICALENDAR_WITH_CURL=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
# 네트워크가 필요한 명시적 확인이며 CTest 자동 실행에는 포함하지 않는다.
./build/iiCalendarHttpsSmoke
```

구현 전 미해결 심벌은 `build/external-tdd-red.log`, 최종 구성·빌드·CTest 결과는
`build/external-configure.log`, `build/external-build.log`, `build/external-ctest.log`에 기록한다.
libcurl OFF 구성에서 호스트 전송 주입과 설치 소비자 결과는 `build/external-no-curl-*.log`에 기록한다.
Apple 제공자의 iOS arm64/iOS 17 API 구문 검사는 `build/external-ios-syntax.log`, 비 Apple 대체 구현의
호스트 컴파일은 `build/external-no-apple-build.log`에 기록한다. iOS 전체 링크·배포·기기 실행 결과는 아니다.

`iiCalendarHttpsSmoke`는 비밀이 아닌 의도적으로 잘못된 fixture 토큰으로 Google 서버의 401 응답을
받아 실제 TLS/HTTPS 경로를 검증한다. 로그인한 Google 계정의 데이터 조회 성공을 증명하지 않는다.
Google 응답 데이터 검사는 주입된 결정적 fixture이며, Apple 실제 계정 목록·일정 조회는 이 테스트에서
수행하지 않는다. 해당 런타임 검증에는 호출 앱의 OAuth 토큰 및 EventKit full-access 권한·호스트 설정이
필요하다. 로그인 UI와 앱 실행 경로 연결은 이 SDK 변경의 검증 범위에 포함하지 않는다.

## 기존 핵심 모듈

2026-09-19 macOS arm64 최종 실행에서 CTest 9/9가 통과했다. 8개 도메인 묶음의 명시적
검사는 532개였고, 나머지 1개 묶음은 설치·외부 소비자 빌드·실행 검증이다. 최종 컴파일 로그에
경고가 없었다. OS가 보고한 벽시계 분해능은 1 µs였다. 이는 UTC 절대 정확도가 아니다.
ICU bootstrap 스크립트의 해시 확인·구성·정적 빌드·설치도 성공했으며, SDK 일괄 설치기
`--dry-run --only iiCalendar`에서 로컬 상위 모듈 의존성 없이 발견되었다.

CTest 8개 도메인 묶음과 설치 소비자 1개로 구성된다. assert를 사용하지 않아 Release에서도
검사가 제거되지 않는다. DB·빌드·설치 산출물은 build/ 아래에 둔다.

| 묶음 | 검증 |
|---|---|
|Precision| 음수 epoch, 18자리 왕복, carry/borrow, overflow, 윤년·ISO 주차, 극단 연도 |
|달력| 등록된 모든 역법 왕복, 율리우스력, 페르시아 신년, 불기, 잘못된 날짜 |
|zones| 서울, 뉴욕 gap/fold, 23/25시간 날, 사모아 생략일, OS 시간대 |
|events| 일정·종일·할 일, 좌표, 완료율, 우선순위, 반복 불변조건 |
|재발| DST, 월말, 주별 요일, 마지막 요일, 제외·추가 회차, 상한, 원본 fold 시점 |
|스토리지| 전체 메타데이터·재개방·attosecond 경계·태그·인덱스 계획·수정 충돌 |
|스토리지| DB 트리거 실패 후 롤백, 독립 연결 동시 수정, 손상 payload 거부 |
|정밀/저장| 전역 로케일과 무관한 소수초, 누적 payload 크기 상한 및 기존 레코드 보존 |
|views| 42셀 월, 윤년, 각 뷰, 음력 월, 일별 집계와 파일 중복 제거 |
|시계| 실제 OS 시각·분해능·단조 시계·관측 구간 |
| installed_consumer | build/install 설치 후 별도 CMake 프로젝트에서 find_package·링크·DB 사용 |

```sh
python3 scripts/bootstrap_icu.py --jobs 4
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 6
ctest --test-dir build --output-on-failure
./build/iiCalendarExample
```

구현 전 계약 테스트의 미해결 심벌 실패는 build/tdd-red.log에 기록했다.
최종 로그는 build/configure.log, build/build.log, build/ctest.log이다.
예제는 2026-09-19 서울 날짜의 일정 1개·할 일 1개·파일 1개와 소수 18자리를 출력한다.

실행 검증 환경은 macOS arm64, Apple Clang 21, ICU 78.3, OS 제공 SQLite이다.
Linux/Android POSIX 시계 분기 및 Windows 분기는 코드에 포함되지만 대상 운영체제 빌드·실행을
검증한 결과는 아니다. iOS/Android/Windows 배포에는 대상 ABI의 ICU·SQLite가 필요하다.
WebAssembly 브라우저 시계·파일시스템 어댑터는 제공하지 않는다.

시계 검사는 하드웨어에서 attosecond 정확도를 인증하지 않는다. 관측 음력·국가별 시각 정책은
사용한 ICU/tzdb 데이터의 범위를 따른다.
