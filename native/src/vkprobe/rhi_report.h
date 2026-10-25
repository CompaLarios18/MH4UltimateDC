// `mh4n_vkprobe --rhi` (N2-30): makes the GPU backend (native/src/gpu/backend/rhi.h) with no surface and
// prints what it reports after the mask, one line per Capabilities field. It gates nothing: the maintainer
// reads it on the reference PC (D57: present_wait is expected false there).
//
// Only this part of the module touches the backend; the JSON reader, the judgement and the plain report
// (report.h, input.cpp, tool.cpp) include no backend or Vulkan header, so `mh4n_vkprobe` without --rhi
// works on a machine with no Vulkan at all.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace mh4n::gpu::rhi {
class Backend;
struct Capabilities;
}  // namespace mh4n::gpu::rhi

namespace mh4n::vkprobe {

struct RhiField {
    std::string name;   // the field's name in Capabilities ("present_wait", "stride_alignment", ...)
    std::string value;  // "true"/"false", a number, a name; the mask as its bit list ("none" when empty)
};

// Every field of Capabilities, in declaration order: the bits the mask can turn off, then the values.
std::vector<RhiField> RhiFields(const mh4n::gpu::rhi::Capabilities& caps);

// "rhi device: <name> (<driver>)", then `rhi <field>: <value>` for every field of RhiFields, the backend's
// `rhi present_wait_available`, `rhi present_wait_measures_display`, `rhi display_timing_available`,
// `rhi surface` and one `rhi extension: <name>` per enabled device extension. Ends with a newline.
std::string RenderRhiReport(const mh4n::gpu::rhi::Backend& backend);

// Makes the instance and the backend and prints the report to `out`. 0 done; 2 an unknown name in
// MH4N_RHI_MASK; 3 no Vulkan loader, no device, or no device the backend accepts (the message on `err`).
int RunRhiProbe(std::ostream& out, std::ostream& err);

}  // namespace mh4n::vkprobe
