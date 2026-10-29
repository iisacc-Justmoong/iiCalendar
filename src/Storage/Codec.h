#pragma once
#include <iiCalendar/Event.h>
#include <span>

namespace iiCalendar::detail {
std::vector<std::byte> encode(const Event &event);
Event decode(std::span<const std::byte> bytes);
} // namespace iiCalendar::detail
