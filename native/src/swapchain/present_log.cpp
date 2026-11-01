// Not derived from Azahar (D57's log format is ours).
#include "swapchain/present_log.h"

#include <ctime>

#include <fmt/format.h>

namespace mh4n::swapchain {

u64 NowUs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string FormatPresentHeader(std::string_view date, std::optional<double> refresh_hz, std::string_view display_time) {
    const std::string refresh = refresh_hz.has_value() && *refresh_hz > 0.0 ? fmt::format("{:.2f}", *refresh_hz) : "unknown";
    return fmt::format("# present v1 {} refresh_hz={} display_time={}\n{}\n", date, refresh, display_time,
                       kPresentColumns);
}

std::string FormatPresentRow(const PresentRow& row) {
    const std::string displayed = row.displayed_us.has_value() ? std::to_string(*row.displayed_us) : std::string();
    return fmt::format("{},{},{},{},{},{},{}\n", row.sequence, row.game_frame, row.published_us, row.composed_us,
                       row.presented_us, displayed, row.dropped_before);
}

std::string UtcDate(std::chrono::system_clock::time_point time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm parts = {};
#ifdef _WIN32
    gmtime_s(&parts, &seconds);
#else
    gmtime_r(&seconds, &parts);
#endif
    return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                       parts.tm_hour, parts.tm_min, parts.tm_sec);
}

} // namespace mh4n::swapchain
