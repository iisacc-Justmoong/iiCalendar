# 소스 책임 분류

공개 헤더와 구현은 `src/` 아래 책임별로 함께 둔다.

- `Chronology`: 달력, 날짜, 시간, 시간대와 내부 ICU 계산.
- `Events`: 일정 모델과 반복 일정.
- `Storage`: SQLite 저장과 직렬화.
- `Presentation`: 화면 표시용 뷰 모델.
- `Integration`: 외부 계정, 제공자, 전송과 공개 진입 헤더.
- `Security`: 내부 SHA-256 계산.

빌드 중 생성되는 forwarding 헤더와 설치된 공개 헤더는 기존 `<iiCalendar/...>` 이름을 유지한다.
공개 API는 소스 디렉터리 이름에 의존하지 않는다. `tests/source_layout.cmake`와 SDK 공통 경계 테스트가 구조 회귀를 검사한다.
