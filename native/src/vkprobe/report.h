// The judged report of one device (N2-24): one line per feature MH4U's rendering needs, with the
// fallback to use when it is missing. Rendered as a text table and as JSON; the JSON reads back.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "vkprobe/facts.h"

namespace mh4n::vkprobe {

struct FeatureResult {
    std::string id;        // stable key: "logicOp", "triangleFans", "D24_UNORM_S8_UINT" ...
    std::string title;     // what the table shows
    Tri status = Tri::Unknown;
    std::string fallback;  // what to do without it (shown when the status is No)
    std::string detail;    // why: the extension that is missing, "core in Vulkan 1.3" ...
};

struct DeviceReport {
    std::string source;  // "live", "vulkaninfo" or "report"
    std::string device_name;
    std::string device_type;
    std::string driver;  // driver name and info, "" when not reported
    std::string api_version;
    u32 vendor_id = 0;
    u32 device_id = 0;
    std::vector<FeatureResult> features;

    const FeatureResult* Find(std::string_view id) const;
};

// Judges one device. Same facts, same report.
DeviceReport Evaluate(const Facts& facts, const std::string& source);

// "yes", "no (fallback: ...)" (just "no" when there is no fallback) or "unknown".
std::string StatusText(const FeatureResult& result);

// The table for one device, ending with a newline.
std::string RenderTable(const DeviceReport& report);
// One JSON document for one device (indented, ending with a newline). ParseReports reads it back.
std::string RenderJson(const DeviceReport& report);

// The devices in the text of a file: `vulkaninfo --json` (one device, or one per capability set)
// or this program's own `--json` (one document per device). False, with a message, when the text is
// not JSON, is truncated, or holds no device.
bool ParseReports(std::string_view text, std::vector<DeviceReport>* reports, std::string* error);

}  // namespace mh4n::vkprobe
