#include "Http.h"
#include <curl/curl.h>
#include <limits>

namespace iiCalendar::detail {
namespace {
struct Body {
    std::string value;
    bool tooLarge = false, allocationFailed = false;
};
std::size_t receive(char *data, std::size_t size, std::size_t count, void *opaque) noexcept {
    auto &body = *static_cast<Body *>(opaque);
    constexpr std::size_t maximum = 16 * 1024 * 1024;
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) { body.tooLarge = true; return 0; }
    const auto length = size * count;
    if (length > maximum - body.value.size()) { body.tooLarge = true; return 0; }
    try { body.value.append(data, length); } catch (...) { body.allocationFailed = true; return 0; }
    return length;
}
}
CalendarHttpGet defaultCalendarHttpGet() {
    return [](const CalendarHttpRequest &request) -> CalendarHttpResponse {
        static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (initialized != CURLE_OK) throw ExternalCalendarError(ExternalCalendarErrorCode::Transport, "could not initialize Calendar HTTPS");
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
        if (!handle) throw ExternalCalendarError(ExternalCalendarErrorCode::Transport, "could not create Calendar HTTPS request");
        curl_slist *rawHeaders = nullptr;
        for (const auto &[name, value] : request.headers) {
            auto *next = curl_slist_append(rawHeaders, (name + ": " + value).c_str());
            if (!next) { curl_slist_free_all(rawHeaders); throw std::bad_alloc(); }
            rawHeaders = next;
        }
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(rawHeaders, curl_slist_free_all);
        Body body;
        auto option = [&](CURLoption key, auto value) {
            if (curl_easy_setopt(handle.get(), key, value) != CURLE_OK)
                throw ExternalCalendarError(ExternalCalendarErrorCode::Transport, "could not configure Calendar HTTPS");
        };
        option(CURLOPT_URL, request.url.c_str());
        option(CURLOPT_HTTPHEADER, headers.get());
        option(CURLOPT_FOLLOWLOCATION, 0L);
        option(CURLOPT_SSL_VERIFYPEER, 1L);
        option(CURLOPT_SSL_VERIFYHOST, 2L);
        option(CURLOPT_PROTOCOLS_STR, "https");
        option(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        option(CURLOPT_TIMEOUT_MS, 30000L);
        option(CURLOPT_NOSIGNAL, 1L);
        option(CURLOPT_WRITEFUNCTION, &receive);
        option(CURLOPT_WRITEDATA, &body);
        const auto result = curl_easy_perform(handle.get());
        if (body.tooLarge) throw ExternalCalendarError(ExternalCalendarErrorCode::LimitExceeded, "Calendar HTTPS response size limit exceeded");
        if (result != CURLE_OK || body.allocationFailed)
            throw ExternalCalendarError(ExternalCalendarErrorCode::Transport, "Calendar HTTPS request failed");
        long status = 0;
        if (curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK)
            throw ExternalCalendarError(ExternalCalendarErrorCode::Transport, "Calendar HTTPS status unavailable");
        return {static_cast<int>(status), std::move(body.value)};
    };
}
} // namespace iiCalendar::detail
