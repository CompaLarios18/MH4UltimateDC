// The present log (N2b-06, D57): a CSV the presenter writes, one row per presented frame, so that N2b-23's report and
// the maintainer's 120-fps procedure can read when each frame was shown. The text of the lines is made here, from plain
// values, so tests check it without a device.
//
//     # present v1 2026-11-01T12:00:00Z refresh_hz=240.00 display_time=present_wait
//     sequence,game_frame,published_us,composed_us,presented_us,displayed_us,dropped_before
//     0,3,1234567,1234890,1235020,1239500,0
//
// All times are microseconds of the steady clock (std::chrono::steady_clock's own epoch), so they compare with the
// other logs of the process. displayed_us is empty when the time a frame was shown is not measurable.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "mh4n/types.h"

namespace mh4n::swapchain {

// Microseconds of the steady clock.
u64 NowUs();

// One row. `sequence` and `game_frame` are the DisplayFrame's own; `published_us` is when Publish got the frame (when
// RequestRedraw asked, for a redraw); `composed_us` when its composition was submitted; `presented_us` when
// vkQueuePresentKHR returned (offscreen: when the GPU finished it); `dropped_before` the frames published since the
// previous row that were never shown (replaced in the mailbox, or discarded while minimised).
struct PresentRow {
    u64 sequence = 0;
    u32 game_frame = 0;
    u64 published_us = 0;
    u64 composed_us = 0;
    u64 presented_us = 0;
    std::optional<u64> displayed_us;
    u64 dropped_before = 0;
};

// "# present v1 <date> refresh_hz=<n.nn|unknown> display_time=<label>\n" and the column line.
std::string FormatPresentHeader(std::string_view date, std::optional<double> refresh_hz, std::string_view display_time);
inline constexpr std::string_view kPresentColumns =
    "sequence,game_frame,published_us,composed_us,presented_us,displayed_us,dropped_before";
std::string FormatPresentRow(const PresentRow& row);

// "2026-11-01T12:00:00Z" (UTC).
std::string UtcDate(std::chrono::system_clock::time_point time);

} // namespace mh4n::swapchain
