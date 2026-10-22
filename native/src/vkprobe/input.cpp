// Reads reports from files (N2-24): `vulkaninfo --json` and this program's own `--json`.
//
// The layout of `vulkaninfo --json` is the Vulkan Profiles device format. This reader assumes
//     { "capabilities": { "device": {
//           "properties": { "VkPhysicalDeviceProperties": { "deviceName": ..., "apiVersion": ..., ... },
//                           "VkPhysicalDeviceDriverProperties": { "driverName": ..., "driverInfo": ... } },
//           "features":   { "VkPhysicalDeviceFeatures": { "logicOp": true, ... }, <one object per structure> },
//           "extensions": { "VK_KHR_swapchain": 70, ... },
//           "formats":    { "VK_FORMAT_D24_UNORM_S8_UINT": { "VkFormatProperties": { "optimalTilingFeatures": [...] } } }
//     } } }
// but it looks keys up by name, accepts the variants seen between SDK versions (the structures
// flattened or nested, extensions as an object, a list of names or a list of objects, feature flags as
// numbers or as lists of names, enumerations as names or numbers) and treats anything absent as unknown.
// Checked against a real file: `vulkaninfo --json` of Vulkan SDK 1.3.275 on Linux (Mesa lavapipe) has this
// layout, with flag lists written as names; other versions and systems have not been seen.
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "vkprobe/json.h"
#include "vkprobe/report.h"

namespace mh4n::vkprobe {

namespace {

constexpr int kFlattenDepth = 4;

// Every member below `object` (the structures are objects of members) by name; the first one wins.
void Flatten(const Json& object, int depth, std::vector<std::pair<std::string_view, const Json*>>* out) {
    if (!object.IsObject() || depth > kFlattenDepth) {
        return;
    }
    for (const JsonMember& m : object.object) {
        if (m.value.IsObject()) {
            Flatten(m.value, depth + 1, out);
        } else {
            out->emplace_back(m.key, &m.value);
        }
    }
}

const Json* FindFlat(const std::vector<std::pair<std::string_view, const Json*>>& flat, std::string_view key) {
    for (const auto& [name, value] : flat) {
        if (name == key) {
            return value;
        }
    }
    return nullptr;
}

std::string StringOf(const Json* j) {
    return j != nullptr && j->IsString() ? j->string : std::string();
}

u32 UIntOf(const Json* j) {
    if (j == nullptr || !j->IsNumber() || j->number < 0 || j->number > 4294967295.0) {
        return 0;
    }
    return static_cast<u32>(j->number);
}

u32 ApiVersionOf(const Json* j) {
    if (j == nullptr) {
        return 0;
    }
    if (j->IsNumber()) {
        return UIntOf(j);
    }
    if (j->IsString()) {  // "1.3.296"
        unsigned parts[3] = {0, 0, 0};
        int n = 0;
        for (const char c : j->string) {
            if (c == '.') {
                if (++n > 2) {
                    return 0;
                }
            } else if (c >= '0' && c <= '9') {
                parts[n] = parts[n] * 10 + static_cast<unsigned>(c - '0');
            } else {
                return 0;
            }
        }
        return (parts[0] << 22) | (parts[1] << 12) | parts[2];
    }
    return 0;
}

std::string DeviceTypeOf(const Json* j) {
    if (j == nullptr) {
        return {};
    }
    static const char* const kNames[] = {"other", "integrated GPU", "discrete GPU", "virtual GPU", "CPU"};
    if (j->IsNumber() && j->number >= 0 && j->number < 5) {
        return kNames[static_cast<int>(j->number)];
    }
    if (j->IsString()) {
        const std::string& s = j->string;
        if (s.find("DISCRETE") != std::string::npos) return kNames[2];
        if (s.find("INTEGRATED") != std::string::npos) return kNames[1];
        if (s.find("VIRTUAL") != std::string::npos) return kNames[3];
        if (s.find("CPU") != std::string::npos) return kNames[4];
        if (s.find("OTHER") != std::string::npos) return kNames[0];
        return s;
    }
    return {};
}

Tri TriOf(const Json& j) {
    if (j.IsBool()) {
        return j.boolean ? Tri::Yes : Tri::No;
    }
    if (j.IsNumber()) {
        return j.number != 0 ? Tri::Yes : Tri::No;
    }
    return Tri::Unknown;
}

void MergeFeature(Facts* facts, const std::string& name, Tri value) {
    if (value == Tri::Unknown) {
        return;
    }
    Tri& slot = facts->features[name];  // Unknown when new
    if (value == Tri::Yes || slot == Tri::Unknown) {
        slot = value;
    }
}

void ReadFeatures(const Json& features, Facts* facts) {
    std::vector<std::pair<std::string_view, const Json*>> flat;
    Flatten(features, 0, &flat);
    for (const auto& [name, value] : flat) {
        MergeFeature(facts, std::string(name), TriOf(*value));
    }
}

void ReadExtensions(const Json& ext, Facts* facts) {
    facts->extensions_known = true;
    if (ext.IsObject()) {
        for (const JsonMember& m : ext.object) {
            facts->extensions.insert(m.key);
        }
    } else if (ext.IsArray()) {
        for (const Json& e : ext.array) {
            if (e.IsString()) {
                facts->extensions.insert(e.string);
            } else if (e.IsObject()) {
                if (const Json* name = e.Find("extensionName"); name != nullptr && name->IsString()) {
                    facts->extensions.insert(name->string);
                }
            }
        }
    }
}

constexpr u64 kFeatureSampledImage = 0x1;
constexpr u64 kFeatureDepthStencilAttachment = 0x200;

bool EndsWith(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The optimalTilingFeatures of a format entry: a bit mask or a list of flag names. False when the
// entry has none.
bool ReadOptimalFeatures(const Json& format, FormatSupport* support, bool* invalid) {
    std::vector<std::pair<std::string_view, const Json*>> flat;
    // the value may be an array, which Flatten keeps as a member
    Flatten(format, 0, &flat);
    bool found = false;
    bool sampled = false;
    bool depth_stencil = false;
    for (const auto& [name, value] : flat) {
        if (name != "optimalTilingFeatures") {
            continue;
        }
        if (value->IsNumber() && !(value->number >= 0 && value->number < 18446744073709551616.0)) {
            *invalid = true;  // negative, NaN or too big for a 64-bit flag mask: casting it would be undefined
            return false;
        }
        if (value->IsNumber()) {
            const u64 bits = static_cast<u64>(value->number);
            found = true;
            sampled = sampled || (bits & kFeatureSampledImage) != 0;
            depth_stencil = depth_stencil || (bits & kFeatureDepthStencilAttachment) != 0;
        } else if (value->IsArray()) {
            found = true;
            for (const Json& flag : value->array) {
                if (!flag.IsString()) {
                    continue;
                }
                sampled = sampled || EndsWith(flag.string, "_SAMPLED_IMAGE_BIT");
                depth_stencil = depth_stencil || EndsWith(flag.string, "_DEPTH_STENCIL_ATTACHMENT_BIT");
            }
        }
    }
    if (found) {
        support->sampled_image = sampled ? Tri::Yes : Tri::No;
        support->depth_stencil_attachment = depth_stencil ? Tri::Yes : Tri::No;
    }
    return found;
}

bool ReadFormats(const Json& formats, Facts* facts, std::string* error) {
    if (!formats.IsObject()) {
        return true;
    }
    facts->formats_listed = true;
    for (const JsonMember& m : formats.object) {
        std::string name = m.key;
        if (name.compare(0, 10, "VK_FORMAT_") == 0) {
            name.erase(0, 10);
        }
        FormatSupport support;
        bool invalid = false;
        if (ReadOptimalFeatures(m.value, &support, &invalid)) {
            facts->formats[name] = support;
        } else if (invalid) {
            *error = "optimalTilingFeatures of " + m.key + " is not a valid flag mask (a number from 0 to 2^64 - 1, or a list of names)";
            return false;
        }
    }
    return true;
}

// One capability set of a vulkaninfo document into `facts`. False when it holds nothing we read.
bool ReadCapabilitySet(const Json& set, Facts* facts, std::string* error) {
    bool any = false;
    if (const Json* props = set.Find("properties"); props != nullptr && props->IsObject()) {
        std::vector<std::pair<std::string_view, const Json*>> flat;
        Flatten(*props, 0, &flat);
        if (facts->device_name.empty()) facts->device_name = StringOf(FindFlat(flat, "deviceName"));
        if (facts->device_type.empty()) facts->device_type = DeviceTypeOf(FindFlat(flat, "deviceType"));
        if (facts->driver_name.empty()) facts->driver_name = StringOf(FindFlat(flat, "driverName"));
        if (facts->driver_info.empty()) facts->driver_info = StringOf(FindFlat(flat, "driverInfo"));
        if (facts->api_version == 0) facts->api_version = ApiVersionOf(FindFlat(flat, "apiVersion"));
        if (facts->vendor_id == 0) facts->vendor_id = UIntOf(FindFlat(flat, "vendorID"));
        if (facts->device_id == 0) facts->device_id = UIntOf(FindFlat(flat, "deviceID"));
        any = true;
    }
    if (const Json* features = set.Find("features"); features != nullptr && features->IsObject()) {
        ReadFeatures(*features, facts);
        any = true;
    }
    if (const Json* ext = set.Find("extensions"); ext != nullptr && (ext->IsObject() || ext->IsArray())) {
        ReadExtensions(*ext, facts);
        any = true;
    }
    if (const Json* formats = set.Find("formats"); formats != nullptr) {
        if (!ReadFormats(*formats, facts, error)) {
            return false;
        }
        any = any || formats->IsObject();
    }
    return any;
}

bool LooksLikeCapabilitySet(const Json& j) {
    return j.IsObject() && (j.Find("properties") != nullptr || j.Find("features") != nullptr ||
                            j.Find("extensions") != nullptr || j.Find("formats") != nullptr);
}

bool ReportFromVulkaninfo(const Json& doc, DeviceReport* report, std::string* error) {
    error->clear();
    Facts facts;
    bool any = false;
    const Json* caps = doc.Find("capabilities");
    if (caps != nullptr && caps->IsObject()) {
        if (LooksLikeCapabilitySet(*caps)) {
            any = ReadCapabilitySet(*caps, &facts, error);
        }
        for (const JsonMember& m : caps->object) {
            if (LooksLikeCapabilitySet(m.value)) {
                any = ReadCapabilitySet(m.value, &facts, error) || any;
            } else if (m.value.IsObject()) {  // capabilities: { name: { "device": {...} } }
                for (const JsonMember& inner : m.value.object) {
                    if (LooksLikeCapabilitySet(inner.value)) {
                        any = ReadCapabilitySet(inner.value, &facts, error) || any;
                    }
                }
            }
        }
    } else if (caps != nullptr && caps->IsArray()) {
        for (const Json& element : caps->array) {
            if (LooksLikeCapabilitySet(element)) {
                any = ReadCapabilitySet(element, &facts, error) || any;
            } else if (const Json* device = element.Find("device"); device != nullptr) {
                any = ReadCapabilitySet(*device, &facts, error) || any;
            }
        }
    } else if (const Json* device = doc.Find("device"); device != nullptr && LooksLikeCapabilitySet(*device)) {
        any = ReadCapabilitySet(*device, &facts, error);
    } else if (LooksLikeCapabilitySet(doc)) {
        any = ReadCapabilitySet(doc, &facts, error);
    }
    if (!error->empty()) {
        return false;
    }
    if (!any) {
        *error = "the document has no device: expected `vulkaninfo --json` output "
                 "(capabilities > device > properties, features, extensions, formats) or this program's --json";
        return false;
    }
    *report = Evaluate(facts, "vulkaninfo");
    return true;
}

bool ReportFromOwn(const Json& doc, DeviceReport* report, std::string* error) {
    const Json* version = doc.Find("mh4n_vkprobe");
    if (version == nullptr || !version->IsNumber() || version->number != 1) {
        *error = "unsupported report version (this program reads \"mh4n_vkprobe\": 1)";
        return false;
    }
    report->source = StringOf(doc.Find("source"));
    if (const Json* device = doc.Find("device"); device != nullptr && device->IsObject()) {
        report->device_name = StringOf(device->Find("name"));
        report->device_type = StringOf(device->Find("type"));
        report->driver = StringOf(device->Find("driver"));
        report->api_version = StringOf(device->Find("api_version"));
        report->vendor_id = UIntOf(device->Find("vendor_id"));
        report->device_id = UIntOf(device->Find("device_id"));
    }
    const Json* features = doc.Find("features");
    if (features == nullptr || !features->IsArray()) {
        *error = "the report has no \"features\" list";
        return false;
    }
    for (const Json& f : features->array) {
        FeatureResult r;
        r.id = StringOf(f.Find("id"));
        r.title = StringOf(f.Find("title"));
        r.fallback = StringOf(f.Find("fallback"));
        r.detail = StringOf(f.Find("detail"));
        const std::string status = StringOf(f.Find("status"));
        if (r.id.empty() || (status != "yes" && status != "no" && status != "unknown")) {
            *error = "a feature of the report has no id or a status other than yes, no or unknown";
            return false;
        }
        if (r.title.empty()) {
            r.title = r.id;
        }
        r.status = status == "yes" ? Tri::Yes : status == "no" ? Tri::No : Tri::Unknown;
        report->features.push_back(std::move(r));
    }
    return true;
}

}  // namespace

bool ParseReports(std::string_view text, std::vector<DeviceReport>* reports, std::string* error) {
    reports->clear();
    std::vector<Json> documents;
    if (!ParseJsonDocuments(text, &documents, error)) {
        return false;
    }
    for (const Json& doc : documents) {
        if (!doc.IsObject()) {
            *error = "a document is not a JSON object";
            return false;
        }
        DeviceReport report;
        const bool ok = doc.Find("mh4n_vkprobe") != nullptr ? ReportFromOwn(doc, &report, error)
                                                             : ReportFromVulkaninfo(doc, &report, error);
        if (!ok) {
            reports->clear();
            return false;
        }
        reports->push_back(std::move(report));
    }
    return true;
}

}  // namespace mh4n::vkprobe
