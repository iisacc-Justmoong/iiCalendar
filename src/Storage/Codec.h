#pragma once
#include <iiCalendarIntegration/Event.h>
#include <span>

namespace iiCalendarIntegration::detail {
std::vector<std::byte> encode(const Event &event);
Event decode(std::span<const std::byte> bytes);
} // namespace iiCalendarIntegration::detail
