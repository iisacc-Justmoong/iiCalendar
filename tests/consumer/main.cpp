#include <iiCalendar/iiCalendar.h>
#include <iostream>

int main() {
    iiCalendar::Store store(":memory:");
    iiCalendar::Event event;
    event.id = "consumer";
    event.kind = iiCalendar::EventKind::Task;
    event.due = iiCalendar::Instant::parse("2026-09-19T00:00:00.000000000000000001Z");
    const auto saved = store.put(event, 0);
    if (store.get(event.id) != saved)
        return 1;
    std::cout << "iiCalendar installed consumer: " << saved.due->toString() << '\n';
    return 0;
}
