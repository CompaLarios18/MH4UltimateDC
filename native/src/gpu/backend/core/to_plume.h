// The translation from the interface's pipeline state to the vendored backend's description (N2-26): one
// function, field by field, and the table from every Format of rhi.h to the vendored backend's formats.
#pragma once

#include <vector>

#include "gpu/backend/rhi.h"
#include "plume_vulkan.h"

namespace mh4n::gpu::rhi::core {

// The vendored backend's format for a Format of rhi.h. The PICA names map to their default storage
// (Rgb8 to RGBA8, Rgb565 to R5G6B5, Rgb5A1 to R5G5B5A1, Rgba4 to R4G4B4A4; the chooser of the targets
// module may pick another format before an image is made). Undefined maps to UNKNOWN.
::plume::RenderFormat ToPlumeFormat(Format format);
// Whether a Format can be a vertex attribute (the vendored backend has it, and it is not a depth,
// compressed or packed 16-bit colour format).
bool IsVertexFormat(Format format);
bool IsDepthFormat(Format format);

// A graphics pipeline description in the vendored backend's terms. The arrays `desc` points to live here,
// so the object does not move or copy once built.
struct PlumeGraphicsDesc {
    ::plume::RenderGraphicsPipelineDesc desc;
    std::vector<::plume::RenderInputSlot> slots;
    std::vector<::plume::RenderInputElement> elements;
    std::vector<::plume::RenderSpecConstant> spec_constants;

    PlumeGraphicsDesc() = default;
    PlumeGraphicsDesc(const PlumeGraphicsDesc&) = delete;
    PlumeGraphicsDesc& operator=(const PlumeGraphicsDesc&) = delete;
};

// FixedFunctionState into `desc`: the blend equations and factors (the constant factors read the dynamic
// blend constants), the colour write mask, the logic op, the depth test, write and compare, the stencil
// state of both faces, the cull mode, the front face and the depth bias. The blend constants and the stencil
// reference and masks are always dynamic (dynamicBlendConstantsEnabled and dynamicStencilEnabled are set), so
// a different value is never another pipeline; depth clipping is on. Errors: a blend factor the vendored
// backend cannot express (the constant-alpha pair), a depth bias constant that is not a whole number (its
// constant factor is an integer).
// `color_attachment` says whether the state's blend applies to a colour attachment.
Result<void> ToPlume(const FixedFunctionState& state, bool color_attachment, ::plume::RenderGraphicsPipelineDesc* desc);

// The whole description: ToPlume(state), the topology, the attachment formats and the vertex layout
// (slots and elements, filled into `out`) and the specialisation constants (one list, `specConstants`; an id
// twice is an error, CheckSpecialization). The shaders and the pipeline layout are the caller's.
Result<void> ToPlume(const GraphicsPipelineDesc& desc, const ::plume::RenderShader* vertex,
                     const ::plume::RenderShader* fragment, const ::plume::RenderShader* geometry,
                     const ::plume::RenderPipelineLayout* layout, PlumeGraphicsDesc* out);

// The pipeline's specialisation list (N2-34) is valid when no id appears twice. The error names the id.
Result<void> CheckSpecialization(const std::vector<SpecConstant>& list);

// One of the interface's enumerations into the vendored backend's.
::plume::RenderComparisonFunction ToPlume(CompareOp op);
::plume::RenderStencilOp ToPlume(StencilOp op);
::plume::RenderBlend ToPlume(BlendFactor factor, bool* expressible);
::plume::RenderBlendOperation ToPlume(BlendOp op);
::plume::RenderLogicOperation ToPlume(LogicOp op);
::plume::RenderPrimitiveTopology ToPlume(Topology topology);
::plume::RenderCullMode ToPlume(CullMode mode);
::plume::RenderFrontFace ToPlume(FrontFace face);

} // namespace mh4n::gpu::rhi::core
