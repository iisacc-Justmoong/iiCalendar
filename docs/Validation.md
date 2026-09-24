# 검증

2026-09-19 macOS arm64 최종 실행에서 CTest 9/9가 통과했다. 8개 도메인 묶음의 명시적
검사는 532개였고, 나머지 1개 묶음은 설치·외부 소비자 빌드·실행 검증이다. 최종 컴파일 로그에
경고가 없었다. OS가 보고한 벽시계 분해능은 1 µs였다. 이는 UTC 절대 정확도가 아니다.
ICU bootstrap 스크립트의 해시 확인·구성·정적 빌드·설치도 성공했으며, SDK 일괄 설치기
`--dry-run --only iiCalendar`에서 로컬 상위 모듈 의존성 없이 발견되었다.

CTest 8개 도메인 묶음과 설치 소비자 1개로 구성된다. assert를 사용하지 않아 Release에서도
검사가 제거되지 않는다. DB·빌드·설치 산출물은 build/ 아래에 둔다.

| 묶음 | 검증 |
|---|---|
| precision | 음수 epoch, 18자리 왕복, carry/borrow, overflow, 윤년·ISO 주차, 극단 연도 |
| calendars | 등록된 모든 역법 왕복, 율리우스력, 페르시아 신년, 불기, 잘못된 날짜 |
| zones | 서울, 뉴욕 gap/fold, 23/25시간 날, 사모아 생략일, OS 시간대 |
| events | 일정·종일·할 일, 좌표, 완료율, 우선순위, 반복 불변조건 |
| recurrence | DST, 월말, 주별 요일, 마지막 요일, 제외·추가 회차, 상한, 원본 fold 시점 |
| storage | 전체 메타데이터·재개방·attosecond 경계·태그·인덱스 계획·수정 충돌 |
| storage | DB 트리거 실패 후 롤백, 독립 연결 동시 수정, 손상 payload 거부 |
| precision / storage | 전역 로케일과 무관한 소수초, 누적 payload 크기 상한 및 기존 레코드 보존 |
| views | 42셀 월, 윤년, 각 뷰, 음력 월, 일별 집계와 파일 중복 제거 |
| clock | 실제 OS 시각·분해능·단조 시계·관측 구간 |
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
