#include "vkprobe/rhi_report.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <ostream>
#include <sstream>

#include "gpu/backend/rhi.h"

namespace mh4n::vkprobe {

namespace {

namespace rhi = mh4n::gpu::rhi;

std::string Bool(bool value) { return value ? "true" : "false"; }

std::string Float(float value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(value));
    return buf;
}

}  // namespace

std::vector<RhiField> RhiFields(const rhi::Capabilities& c) {
    const std::string masked = c.masked.ToString();
    return {
        {"d24s8", Bool(c.d24s8)},
        {"interlock", Bool(c.interlock)},
        {"index_uint8", Bool(c.index_uint8)},
        {"custom_border_color", Bool(c.custom_border_color)},
        {"triangle_fans", Bool(c.triangle_fans)},
        {"logic_op", Bool(c.logic_op)},
        {"dynamic_rendering", Bool(c.dynamic_rendering)},
        {"pipeline_library", Bool(c.pipeline_library)},
        {"eds3_blend", Bool(c.eds3_blend)},
        {"bc7", Bool(c.bc7)},
        {"timeline", Bool(c.timeline)},
        {"present_wait", Bool(c.present_wait)},
        {"direct_upload", Bool(c.direct_upload)},
        {"geometry_shader", Bool(c.geometry_shader)},
        {"stride_alignment", std::to_string(c.stride_alignment)},
        {"apple", Bool(c.apple)},
        {"primitive_restart_always_on", Bool(c.primitive_restart_always_on)},
        {"portability_subset", Bool(c.portability_subset)},
        {"driver_id", std::to_string(c.driver_id)},
        {"driver_name", c.driver_name},
        {"device_name", c.device_name},
        {"api_version", std::to_string(c.api_version)},
        {"timestamp_period", Float(c.timestamp_period)},
        {"max_texel_buffer_elements", std::to_string(c.max_texel_buffer_elements)},
        {"uniform_buffer_offset_alignment", std::to_string(c.uniform_buffer_offset_alignment)},
        {"storage_buffer_offset_alignment", std::to_string(c.storage_buffer_offset_alignment)},
        {"max_uniform_buffer_range", std::to_string(c.max_uniform_buffer_range)},
        {"max_storage_buffer_range", std::to_string(c.max_storage_buffer_range)},
        {"vendor_id", std::to_string(c.vendor_id)},
        {"device_index", std::to_string(c.device_index)},
        {"masked", masked.empty() ? "none" : masked},
    };
}

std::string RenderRhiReport(const rhi::Backend& backend) {
    const rhi::Capabilities& caps = backend.Caps();
    std::ostringstream out;
    out << "rhi device: " << caps.device_name << " (" << caps.driver_name << ")\n";
    for (const RhiField& field : RhiFields(caps)) {
        out << "rhi " << field.name << ": " << field.value << "\n";
    }
    out << "rhi present_wait_available: " << Bool(backend.PresentWaitAvailable()) << "\n";
    out << "rhi present_wait_measures_display: " << Bool(backend.PresentWaitMeasuresDisplay()) << "\n";
    out << "rhi display_timing_available: " << Bool(backend.DisplayTimingAvailable()) << "\n";
    out << "rhi surface: " << Bool(backend.HasSurface()) << "\n";
    for (const std::string& name : backend.EnabledExtensions()) {
        out << "rhi extension: " << name << "\n";
    }
    return out.str();
}

int RunRhiProbe(std::ostream& out, std::ostream& err) {
    // An unknown name in MH4N_RHI_MASK is a bad argument, not a missing device.
    if (const char* mask = std::getenv("MH4N_RHI_MASK")) {
        const rhi::Result<rhi::CapabilityMask> parsed = rhi::CapabilityMask::Parse(mask);
        if (!parsed) {
            err << "mh4n_vkprobe: --rhi: MH4N_RHI_MASK: " << parsed.error() << "\n";
            return 2;
        }
    }
    const rhi::Result<std::shared_ptr<rhi::Instance>> instance = rhi::CreateInstance();
    if (!instance) {
        err << "mh4n_vkprobe: --rhi: " << instance.error() << "\n"
            << "mh4n_vkprobe: nothing to report; without --rhi the probe still reads `--from-json FILE`\n";
        return 3;
    }
    const rhi::Result<std::shared_ptr<rhi::Backend>> backend = rhi::CreateBackend(*instance);
    if (!backend) {
        err << "mh4n_vkprobe: --rhi: " << backend.error() << "\n"
            << "mh4n_vkprobe: nothing to report; without --rhi the probe still reads `--from-json FILE`\n";
        return 3;
    }
    out << RenderRhiReport(**backend);
    return 0;
}

}  // namespace mh4n::vkprobe
