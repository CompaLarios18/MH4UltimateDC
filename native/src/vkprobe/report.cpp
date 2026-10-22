#include "vkprobe/report.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <tuple>
#include <utility>

#include <fmt/format.h>

#include "vkprobe/json.h"

namespace mh4n::vkprobe {

std::string ApiVersionString(u32 version) {
    if (version == 0) {
        return {};
    }
    // VK_API_VERSION_MAJOR/MINOR/PATCH; the variant is the top three bits
    return fmt::format("{}.{}.{}", (version >> 22) & 0x7F, (version >> 12) & 0x3FF, version & 0xFFF);
}

const FeatureResult* DeviceReport::Find(std::string_view id) const {
    for (const FeatureResult& f : features) {
        if (f.id == id) {
            return &f;
        }
    }
    return nullptr;
}

namespace {

constexpr u32 MakeApi(u32 major, u32 minor) {
    return (major << 22) | (minor << 12);
}

class Judge {
public:
    explicit Judge(const Facts& facts) : f_(facts) {}

    // Is one of these extensions on the device? Unknown when the list was not in the input.
    Tri Ext(std::initializer_list<const char*> names) const {
        for (const char* name : names) {
            if (f_.extensions.count(name) != 0) {
                return Tri::Yes;
            }
        }
        return f_.extensions_known ? Tri::No : Tri::Unknown;
    }

    Tri Feature(std::string_view name) const {
        const auto it = f_.features.find(name);
        return it == f_.features.end() ? Tri::Unknown : it->second;
    }

    bool ApiAtLeast(u32 major, u32 minor) const {
        return f_.api_version >= MakeApi(major, minor);
    }
    bool ApiKnown() const { return f_.api_version != 0; }

    // An extension and the feature bit it adds. Yes needs both: the extension on the device and the
    // bit true. A feature bit alone is not enough (the structure may belong to a layer or another
    // extension), and a name the device did not list is never a yes.
    FeatureResult ExtFeature(std::string id, std::string title, std::initializer_list<const char*> names,
                             std::string_view feature, std::string fallback) const {
        const Tri ext = Ext(names);
        const Tri bit = Feature(feature);
        FeatureResult r = Make(std::move(id), std::move(title), std::move(fallback));
        const std::string ext_name = *names.begin();
        if (ext == Tri::No) {
            r.status = Tri::No;
            r.detail = ext_name + " is not on the device";
        } else if (bit == Tri::No) {
            r.status = Tri::No;
            r.detail = std::string(feature) + " is false";
        } else if (ext == Tri::Yes && bit == Tri::Yes) {
            r.status = Tri::Yes;
        } else {
            r.status = Tri::Unknown;
            r.detail = ext == Tri::Unknown ? "the extension list was not reported"
                                           : std::string(feature) + " was not reported";
        }
        return r;
    }

    // A core feature bit of VkPhysicalDeviceFeatures.
    FeatureResult Core(std::string id, std::string title, std::string_view feature, std::string fallback) const {
        FeatureResult r = Make(std::move(id), std::move(title), std::move(fallback));
        r.status = Feature(feature);
        if (r.status == Tri::Unknown) {
            r.detail = std::string(feature) + " was not reported";
        } else if (r.status == Tri::No) {
            r.detail = std::string(feature) + " is false";
        }
        return r;
    }

    // Promoted to core in Vulkan `major.minor`, where it is a feature bit (timeline semaphores in
    // 1.2, dynamic rendering in 1.3): the bit is the answer. Without it: not on the device when the
    // device is older and does not list the extension.
    FeatureResult Promoted(std::string id, std::string title, std::string_view feature, unsigned major,
                           unsigned minor, std::initializer_list<const char*> names, std::string fallback) const {
        FeatureResult r = Make(std::move(id), std::move(title), std::move(fallback));
        r.status = Feature(feature);
        if (r.status == Tri::Unknown) {
            if (Ext(names) == Tri::No && ApiKnown() && !ApiAtLeast(major, minor)) {
                r.status = Tri::No;
                r.detail = fmt::format("older than Vulkan {}.{} and {} is not on the device", major, minor,
                                       *names.begin());
            } else {
                r.detail = std::string(feature) + " was not reported";
            }
        } else if (r.status == Tri::No) {
            r.detail = std::string(feature) + " is false";
        }
        return r;
    }

    FeatureResult Plain(std::string id, std::string title, Tri status, std::string fallback,
                        std::string detail = {}) const {
        FeatureResult r = Make(std::move(id), std::move(title), std::move(fallback));
        r.status = status;
        r.detail = std::move(detail);
        return r;
    }

    // How many members that start with `prefix` are true, and whether any is reported at all.
    Tri CountFeaturePrefix(std::string_view prefix, int* yes) const {
        bool any = false;
        *yes = 0;
        for (const auto& [name, value] : f_.features) {
            if (name.compare(0, prefix.size(), prefix) != 0 || value == Tri::Unknown) {
                continue;
            }
            any = true;
            *yes += value == Tri::Yes ? 1 : 0;
        }
        return !any ? Tri::Unknown : *yes > 0 ? Tri::Yes : Tri::No;
    }

private:
    static FeatureResult Make(std::string id, std::string title, std::string fallback) {
        FeatureResult r;
        r.id = std::move(id);
        r.title = std::move(title);
        r.fallback = std::move(fallback);
        return r;
    }
    const Facts& f_;
};

Tri FormatTri(const Facts& f, const std::string& name, Tri FormatSupport::*member) {
    const auto it = f.formats.find(name);
    return it == f.formats.end() ? Tri::Unknown : it->second.*member;
}

}  // namespace

DeviceReport Evaluate(const Facts& facts, const std::string& source) {
    DeviceReport report;
    report.source = source;
    report.device_name = facts.device_name;
    report.device_type = facts.device_type;
    report.api_version = ApiVersionString(facts.api_version);
    report.vendor_id = facts.vendor_id;
    report.device_id = facts.device_id;
    report.driver = facts.driver_name;
    if (!facts.driver_info.empty()) {
        report.driver += (report.driver.empty() ? "" : " ") + facts.driver_info;
    }

    const Judge j(facts);
    auto& out = report.features;

    out.push_back(j.Core("logicOp", "logic ops (logicOp)", "logicOp",
                         "emulate the ops in the fragment shader (partial: 4 of 16 ops on MoltenVK)"));
    out.push_back(j.ExtFeature("fragmentShaderPixelInterlock", "fragment shader interlock, pixel",
                               {"VK_EXT_fragment_shader_interlock"}, "fragmentShaderPixelInterlock",
                               "order overlapping draws with barriers between them"));
    out.push_back(j.ExtFeature("fragmentShaderSampleInterlock", "fragment shader interlock, sample",
                               {"VK_EXT_fragment_shader_interlock"}, "fragmentShaderSampleInterlock",
                               "order overlapping draws with barriers between them"));
    out.push_back(j.Core("fragmentStoresAndAtomics", "storage image writes from fragment shaders",
                         "fragmentStoresAndAtomics", "draw into an attachment instead of a storage image"));
    out.push_back(j.Core("geometryShader", "geometry shaders", "geometryShader",
                         "skip the per-triangle quaternion fix-up, or run it in a compute pass"));
    out.push_back(j.ExtFeature("fragmentShaderBarycentric", "fragment shader barycentrics",
                               {"VK_KHR_fragment_shader_barycentric", "VK_NV_fragment_shader_barycentric"},
                               "fragmentShaderBarycentric",
                               "geometry shaders for the quaternion fix-up, else skip it"));

    {
        // An explicit triangleFans: false is a no whatever the extension list says; no portability
        // subset extension on a device whose list is known means a full device, where fans are core.
        const Tri subset = j.Ext({"VK_KHR_portability_subset"});
        const Tri bit = j.Feature("triangleFans");
        const char* fallback = "rewrite fans as triangle lists on the CPU";
        Tri status = Tri::Unknown;
        std::string detail;
        if (bit == Tri::No) {
            status = Tri::No;
            detail = subset == Tri::Yes ? "portability subset: triangleFans is false" : "triangleFans is false";
        } else if (subset == Tri::No) {
            status = Tri::Yes;
            detail = "a full Vulkan device: fans are core";
        } else if (subset == Tri::Yes) {
            status = bit;
            detail = bit == Tri::Unknown ? "VK_KHR_portability_subset is on the device but triangleFans was not reported"
                                         : "portability subset: triangleFans is true";
        } else {
            status = bit;
            detail = bit == Tri::Unknown ? "the extension list was not reported" : "";
        }
        out.push_back(j.Plain("triangleFans", "triangle fans", status, fallback, detail));
    }

    out.push_back(j.ExtFeature("indexTypeUint8", "8-bit indices (VK_EXT_index_type_uint8)",
                               {"VK_EXT_index_type_uint8", "VK_KHR_index_type_uint8"}, "indexTypeUint8",
                               "widen the indices to 16 bits on the CPU"));
    out.push_back(j.ExtFeature("customBorderColors", "custom border colours (VK_EXT_custom_border_color)",
                               {"VK_EXT_custom_border_color"}, "customBorderColors",
                               "emulate the border colour in the shader"));
    out.push_back(j.Core("shaderClipDistance", "shaderClipDistance", "shaderClipDistance",
                         "clip in the fragment shader (discard)"));
    {
        const Tri ext = j.Ext({"VK_EXT_shader_stencil_export"});
        out.push_back(j.Plain("shaderStencilExport", "VK_EXT_shader_stencil_export", ext,
                              "write the stencil in a second pass",
                              ext == Tri::No        ? "VK_EXT_shader_stencil_export is not on the device"
                              : ext == Tri::Unknown ? "the extension list was not reported"
                                                    : ""));
    }
    out.push_back(j.ExtFeature("rasterizationOrderAttachmentAccess",
                               "VK_EXT_rasterization_order_attachment_access",
                               {"VK_EXT_rasterization_order_attachment_access",
                                "VK_ARM_rasterization_order_attachment_access"},
                               "rasterizationOrderColorAttachmentAccess",
                               "fragment shader interlock, else a copy and a barrier per draw"));

        // vulkaninfo leaves out a format the device has no feature for: absent from a file that has a
        // formats section most likely means no, but only the live probe can say so
        const std::string omitted =
            "not listed in the file; vulkaninfo omits formats the device has no feature for, so this most likely "
            "means no; the live probe settles it";
    {
        const auto not_reported = [&]() { return facts.formats_listed ? omitted : std::string("the format was not reported"); };
        const Tri d24 = FormatTri(facts, "D24_UNORM_S8_UINT", &FormatSupport::depth_stencil_attachment);
        const Tri d32 = FormatTri(facts, "D32_SFLOAT_S8_UINT", &FormatSupport::depth_stencil_attachment);
        out.push_back(j.Plain("D24_UNORM_S8_UINT", "depth-stencil D24_UNORM_S8_UINT", d24, "D32_SFLOAT_S8_UINT",
                              d24 == Tri::Unknown ? not_reported()
                              : d24 == Tri::No    ? std::string("D32_SFLOAT_S8_UINT is ") +
                                                       (d32 == Tri::Yes  ? "available"
                                                        : d32 == Tri::No ? "also missing"
                                                                         : "not reported")
                                                  : ""));
        out.push_back(j.Plain("D32_SFLOAT_S8_UINT", "depth-stencil D32_SFLOAT_S8_UINT", d32, "",
                              d32 == Tri::Unknown ? not_reported() : std::string()));
    }
    {
        Tri bc7 = FormatTri(facts, "BC7_UNORM_BLOCK", &FormatSupport::sampled_image);
        std::string detail;
        if (bc7 == Tri::Unknown) {
            bc7 = j.Feature("textureCompressionBC");
            detail = bc7 == Tri::Unknown ? (facts.formats_listed ? omitted + " (textureCompressionBC is not reported either)"
                                                                : std::string("the format and textureCompressionBC were not reported"))
                                         : "from textureCompressionBC";
        }
        out.push_back(j.Plain("BC7", "BC7 textures (BC7_UNORM_BLOCK)", bc7,
                              "decode to RGBA8 at load (four times the memory)", detail));
    }

    out.push_back(j.Promoted("dynamicRendering", "dynamic rendering", "dynamicRendering", 1, 3,
                             {"VK_KHR_dynamic_rendering"}, "render passes and framebuffers"));
    out.push_back(j.ExtFeature("graphicsPipelineLibrary", "VK_EXT_graphics_pipeline_library",
                               {"VK_EXT_graphics_pipeline_library"}, "graphicsPipelineLibrary",
                               "a pipeline cache and compilation on worker threads"));
    // The device's answer: a swapchain's support (surface dependent) cannot be told without a surface.
    for (const auto& [id, ext_name, bit] : {std::tuple<const char*, const char*, const char*>{"presentId", "VK_KHR_present_id", "presentId"},
                                            {"presentWait", "VK_KHR_present_wait", "presentWait"}}) {
        FeatureResult r = j.ExtFeature(id, std::string(ext_name) + " (" + bit + ")", {ext_name}, bit,
                                       "estimate when a frame is shown from the present call (not measured)");
        r.detail += std::string(r.detail.empty() ? "" : "; ") + "device level: a swapchain's support needs a surface";
        out.push_back(std::move(r));
    }
    {
        const Tri ext = j.Ext({"VK_KHR_push_descriptor"});
        Tri status = ext;
        std::string detail;
        if (ext == Tri::No && j.ApiAtLeast(1, 4)) {
            status = Tri::Yes;
            detail = "core in Vulkan 1.4";
        } else if (ext == Tri::No) {
            detail = "VK_KHR_push_descriptor is not on the device";
        } else if (ext == Tri::Unknown) {
            detail = "the extension list was not reported";
        }
        out.push_back(j.Plain("pushDescriptor", "push descriptors (VK_KHR_push_descriptor)", status,
                              "descriptor sets updated per draw", detail));
    }
    out.push_back(j.Promoted("timelineSemaphore", "timeline semaphores", "timelineSemaphore", 1, 2,
                             {"VK_KHR_timeline_semaphore"}, "binary semaphores and fences"));

    {
        FeatureResult eds = j.ExtFeature("extendedDynamicState", "VK_EXT_extended_dynamic_state (1)",
                                         {"VK_EXT_extended_dynamic_state"}, "extendedDynamicState",
                                         "bake the state into pipelines (more pipeline variants)");
        if (eds.status != Tri::Yes && j.ApiAtLeast(1, 3)) {
            eds.status = Tri::Yes;
            eds.detail = "core in Vulkan 1.3";
        }
        out.push_back(std::move(eds));
        FeatureResult eds2 = j.ExtFeature("extendedDynamicState2", "VK_EXT_extended_dynamic_state2",
                                          {"VK_EXT_extended_dynamic_state2"}, "extendedDynamicState2",
                                          "bake the state into pipelines (more pipeline variants)");
        if (eds2.status != Tri::Yes && j.ApiAtLeast(1, 3)) {
            eds2.status = Tri::Yes;
            eds2.detail = "core in Vulkan 1.3";
        }
        out.push_back(std::move(eds2));
        const Tri ext3 = j.Ext({"VK_EXT_extended_dynamic_state3"});
        int yes_bits = 0;
        const Tri bits = j.CountFeaturePrefix("extendedDynamicState3", &yes_bits);
        Tri status = Tri::Unknown;
        std::string detail;
        if (ext3 == Tri::No) {
            status = Tri::No;
            detail = "VK_EXT_extended_dynamic_state3 is not on the device";
        } else if (bits == Tri::No) {
            status = Tri::No;
            detail = "every extendedDynamicState3* bit is false";
        } else if (ext3 == Tri::Yes && bits == Tri::Yes) {
            status = Tri::Yes;
            detail = std::to_string(yes_bits) + " of 31 bits (see the device's extendedDynamicState3* features)";
        } else {
            detail = ext3 == Tri::Unknown ? "the extension list was not reported"
                                          : "the extendedDynamicState3* bits were not reported";
        }
        out.push_back(j.Plain("extendedDynamicState3", "VK_EXT_extended_dynamic_state3", status,
                              "bake the state into pipelines (more pipeline variants)", detail));
    }
    return report;
}

std::string StatusText(const FeatureResult& r) {
    switch (r.status) {
    case Tri::Yes:
        return "yes";
    case Tri::No:
        return r.fallback.empty() ? "no" : "no (fallback: " + r.fallback + ")";
    case Tri::Unknown:
        break;
    }
    return "unknown";
}

std::string RenderTable(const DeviceReport& r) {
    std::string out;
    out += "device: " + (r.device_name.empty() ? std::string("(unnamed)") : r.device_name) + "\n";
    out += "  type: " + (r.device_type.empty() ? std::string("unknown") : r.device_type);
    if (r.vendor_id != 0 || r.device_id != 0) {
        out += fmt::format(" (vendor 0x{:04x}, device 0x{:04x})", r.vendor_id, r.device_id);
    }
    out += "\n";
    out += "  driver: " + (r.driver.empty() ? std::string("unknown") : r.driver) + "\n";
    out += "  api version: " + (r.api_version.empty() ? std::string("unknown") : r.api_version) + "\n";
    out += "  source: " + r.source + "\n";
    out += "  features:\n";
    size_t width = 0;
    for (const FeatureResult& f : r.features) {
        width = std::max(width, f.title.size());
    }
    for (const FeatureResult& f : r.features) {
        out += fmt::format("    {:<{}}  {}", f.title, width, StatusText(f));
        if (!f.detail.empty()) {
            out += "  [" + f.detail + "]";
        }
        out += "\n";
    }
    return out;
}

std::string RenderJson(const DeviceReport& r) {
    std::string out = "{\n";
    out += "  \"mh4n_vkprobe\": 1,\n";
    out += "  \"source\": " + JsonQuote(r.source) + ",\n";
    out += "  \"device\": {\n";
    out += "    \"name\": " + JsonQuote(r.device_name) + ",\n";
    out += "    \"type\": " + JsonQuote(r.device_type) + ",\n";
    out += "    \"driver\": " + JsonQuote(r.driver) + ",\n";
    out += "    \"api_version\": " + JsonQuote(r.api_version) + ",\n";
    out += fmt::format("    \"vendor_id\": {},\n    \"device_id\": {}\n  }},\n", r.vendor_id, r.device_id);
    out += "  \"features\": [\n";
    for (size_t i = 0; i < r.features.size(); ++i) {
        const FeatureResult& f = r.features[i];
        const char* status = f.status == Tri::Yes ? "yes" : f.status == Tri::No ? "no" : "unknown";
        out += "    {\"id\": " + JsonQuote(f.id) + ", \"status\": \"" + status + "\"";
        out += ", \"title\": " + JsonQuote(f.title);
        if (!f.fallback.empty()) {
            out += ", \"fallback\": " + JsonQuote(f.fallback);
        }
        if (!f.detail.empty()) {
            out += ", \"detail\": " + JsonQuote(f.detail);
        }
        out += i + 1 < r.features.size() ? "},\n" : "}\n";
    }
    out += "  ]\n}\n";
    return out;
}

}  // namespace mh4n::vkprobe
