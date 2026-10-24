#include "gpu/backend/core/to_plume.h"

#include <cmath>

#include <fmt/format.h>

namespace mh4n::gpu::rhi::core {

using ::plume::RenderFormat;

RenderFormat ToPlumeFormat(Format format) {
    switch (format) {
    case Format::Undefined: return RenderFormat::UNKNOWN;
    case Format::Rgb8: return RenderFormat::R8G8B8A8_UNORM;
    case Format::Rgb565: return RenderFormat::R5G6B5_UNORM;
    case Format::Rgb5A1: return RenderFormat::R5G5B5A1_UNORM;
    case Format::Rgba4: return RenderFormat::R4G4B4A4_UNORM;
    case Format::R8Unorm: return RenderFormat::R8_UNORM;
    case Format::Rg8Unorm: return RenderFormat::R8G8_UNORM;
    case Format::Rgba8Unorm: return RenderFormat::R8G8B8A8_UNORM;
    case Format::Rgba8Snorm: return RenderFormat::R8G8B8A8_SNORM;
    case Format::Bgra8Unorm: return RenderFormat::B8G8R8A8_UNORM;
    case Format::R5G6B5: return RenderFormat::R5G6B5_UNORM;
    case Format::B5G6R5: return RenderFormat::B5G6R5_UNORM;
    case Format::R5G5B5A1: return RenderFormat::R5G5B5A1_UNORM;
    case Format::B5G5R5A1: return RenderFormat::B5G5R5A1_UNORM;
    case Format::A1R5G5B5: return RenderFormat::A1R5G5B5_UNORM;
    case Format::R4G4B4A4: return RenderFormat::R4G4B4A4_UNORM;
    case Format::B4G4R4A4: return RenderFormat::B4G4R4A4_UNORM;
    case Format::R32Uint: return RenderFormat::R32_UINT;
    case Format::Rgba16Float: return RenderFormat::R16G16B16A16_FLOAT;
    case Format::Bc7Unorm: return RenderFormat::BC7_UNORM;
    case Format::D16Unorm: return RenderFormat::D16_UNORM;
    case Format::D24S8: return RenderFormat::D24_UNORM_S8_UINT;
    case Format::D32Float: return RenderFormat::D32_FLOAT;
    case Format::D32S8: return RenderFormat::D32_FLOAT_S8_UINT;
    case Format::R32Float: return RenderFormat::R32_FLOAT;
    case Format::Rg32Float: return RenderFormat::R32G32_FLOAT;
    case Format::Rgb32Float: return RenderFormat::R32G32B32_FLOAT;
    case Format::Rgba32Float: return RenderFormat::R32G32B32A32_FLOAT;
    case Format::R8Uint: return RenderFormat::R8_UINT;
    case Format::R8Sint: return RenderFormat::R8_SINT;
    case Format::Rg8Uint: return RenderFormat::R8G8_UINT;
    case Format::Rg8Sint: return RenderFormat::R8G8_SINT;
    case Format::Rgba8Uint: return RenderFormat::R8G8B8A8_UINT;
    case Format::Rgba8Sint: return RenderFormat::R8G8B8A8_SINT;
    case Format::R16Uint: return RenderFormat::R16_UINT;
    case Format::R16Sint: return RenderFormat::R16_SINT;
    case Format::Rg16Sint: return RenderFormat::R16G16_SINT;
    case Format::Rgba16Sint: return RenderFormat::R16G16B16A16_SINT;
    }
    return RenderFormat::UNKNOWN;
}

bool IsDepthFormat(Format format) {
    return format == Format::D16Unorm || format == Format::D24S8 || format == Format::D32Float || format == Format::D32S8;
}

bool IsVertexFormat(Format format) {
    switch (format) {
    case Format::R8Unorm:
    case Format::Rg8Unorm:
    case Format::Rgba8Unorm:
    case Format::Rgba8Snorm:
    case Format::Rgba16Float:
    case Format::R32Float:
    case Format::Rg32Float:
    case Format::Rgb32Float:
    case Format::Rgba32Float:
    case Format::R8Uint:
    case Format::R8Sint:
    case Format::Rg8Uint:
    case Format::Rg8Sint:
    case Format::Rgba8Uint:
    case Format::Rgba8Sint:
    case Format::R16Uint:
    case Format::R16Sint:
    case Format::Rg16Sint:
    case Format::Rgba16Sint:
        return true;
    default:
        return false;
    }
}

::plume::RenderComparisonFunction ToPlume(CompareOp op) {
    using F = ::plume::RenderComparisonFunction;
    switch (op) {
    case CompareOp::Never: return F::NEVER;
    case CompareOp::Less: return F::LESS;
    case CompareOp::Equal: return F::EQUAL;
    case CompareOp::LessEqual: return F::LESS_EQUAL;
    case CompareOp::Greater: return F::GREATER;
    case CompareOp::NotEqual: return F::NOT_EQUAL;
    case CompareOp::GreaterEqual: return F::GREATER_EQUAL;
    case CompareOp::Always: return F::ALWAYS;
    }
    return F::UNKNOWN;
}

::plume::RenderStencilOp ToPlume(StencilOp op) {
    using S = ::plume::RenderStencilOp;
    switch (op) {
    case StencilOp::Keep: return S::KEEP;
    case StencilOp::Zero: return S::ZERO;
    case StencilOp::Replace: return S::REPLACE;
    case StencilOp::IncrementClamp: return S::INCREMENT_AND_CLAMP;
    case StencilOp::DecrementClamp: return S::DECREMENT_AND_CLAMP;
    case StencilOp::Invert: return S::INVERT;
    case StencilOp::IncrementWrap: return S::INCREMENT_AND_WRAP;
    case StencilOp::DecrementWrap: return S::DECREMENT_AND_WRAP;
    }
    return S::UNKNOWN;
}

::plume::RenderBlend ToPlume(BlendFactor factor, bool* expressible) {
    using B = ::plume::RenderBlend;
    *expressible = true;
    switch (factor) {
    case BlendFactor::Zero: return B::ZERO;
    case BlendFactor::One: return B::ONE;
    case BlendFactor::SrcColor: return B::SRC_COLOR;
    case BlendFactor::OneMinusSrcColor: return B::INV_SRC_COLOR;
    case BlendFactor::DstColor: return B::DEST_COLOR;
    case BlendFactor::OneMinusDstColor: return B::INV_DEST_COLOR;
    case BlendFactor::SrcAlpha: return B::SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return B::INV_SRC_ALPHA;
    case BlendFactor::DstAlpha: return B::DEST_ALPHA;
    case BlendFactor::OneMinusDstAlpha: return B::INV_DEST_ALPHA;
    case BlendFactor::ConstantColor: return B::BLEND_FACTOR;
    case BlendFactor::OneMinusConstantColor: return B::INV_BLEND_FACTOR;
    case BlendFactor::SrcAlphaSaturate: return B::SRC_ALPHA_SAT;
    case BlendFactor::ConstantAlpha:
    case BlendFactor::OneMinusConstantAlpha:
        break;
    }
    *expressible = false;
    return B::UNKNOWN;
}

::plume::RenderBlendOperation ToPlume(BlendOp op) {
    using O = ::plume::RenderBlendOperation;
    switch (op) {
    case BlendOp::Add: return O::ADD;
    case BlendOp::Subtract: return O::SUBTRACT;
    case BlendOp::ReverseSubtract: return O::REV_SUBTRACT;
    case BlendOp::Min: return O::MIN;
    case BlendOp::Max: return O::MAX;
    }
    return O::UNKNOWN;
}

::plume::RenderLogicOperation ToPlume(LogicOp op) {
    using L = ::plume::RenderLogicOperation;
    switch (op) {
    case LogicOp::Clear: return L::CLEAR;
    case LogicOp::Set: return L::SET;
    case LogicOp::Copy: return L::COPY;
    case LogicOp::CopyInverted: return L::COPY_INVERTED;
    case LogicOp::NoOp: return L::NOOP;
    case LogicOp::Invert: return L::INVERT;
    case LogicOp::And: return L::AND;
    case LogicOp::Nand: return L::NAND;
    case LogicOp::Or: return L::OR;
    case LogicOp::Nor: return L::NOR;
    case LogicOp::Xor: return L::XOR;
    case LogicOp::Equiv: return L::EQUIV;
    case LogicOp::AndReverse: return L::AND_REVERSE;
    case LogicOp::AndInverted: return L::AND_INVERTED;
    case LogicOp::OrReverse: return L::OR_REVERSE;
    case LogicOp::OrInverted: return L::OR_INVERTED;
    }
    return L::UNKNOWN;
}

::plume::RenderPrimitiveTopology ToPlume(Topology topology) {
    using T = ::plume::RenderPrimitiveTopology;
    switch (topology) {
    case Topology::PointList: return T::POINT_LIST;
    case Topology::LineList: return T::LINE_LIST;
    case Topology::LineStrip: return T::LINE_STRIP;
    case Topology::TriangleList: return T::TRIANGLE_LIST;
    case Topology::TriangleStrip: return T::TRIANGLE_STRIP;
    case Topology::TriangleFan: return T::TRIANGLE_FAN;
    }
    return T::UNKNOWN;
}

::plume::RenderCullMode ToPlume(CullMode mode) {
    using C = ::plume::RenderCullMode;
    switch (mode) {
    case CullMode::None: return C::NONE;
    case CullMode::Front: return C::FRONT;
    case CullMode::Back: return C::BACK;
    }
    return C::UNKNOWN;
}

::plume::RenderFrontFace ToPlume(FrontFace face) {
    return face == FrontFace::Clockwise ? ::plume::RenderFrontFace::CLOCKWISE : ::plume::RenderFrontFace::COUNTER_CLOCKWISE;
}

Result<void> ToPlume(const FixedFunctionState& state, bool color_attachment, ::plume::RenderGraphicsPipelineDesc* desc) {
    // The blend equation.
    ::plume::RenderBlendDesc blend;
    blend.blendEnabled = state.blend_enabled;
    bool ok_src_color = false, ok_dst_color = false, ok_src_alpha = false, ok_dst_alpha = false;
    blend.srcBlend = ToPlume(state.src_color, &ok_src_color);
    blend.dstBlend = ToPlume(state.dst_color, &ok_dst_color);
    blend.srcBlendAlpha = ToPlume(state.src_alpha, &ok_src_alpha);
    blend.dstBlendAlpha = ToPlume(state.dst_alpha, &ok_dst_alpha);
    if (state.blend_enabled && !(ok_src_color && ok_dst_color && ok_src_alpha && ok_dst_alpha)) {
        return Error{"a constant-alpha blend factor cannot be expressed by the vendored backend (it maps only the constant-colour pair); "
                     "a patch of the queue is needed"};
    }
    blend.blendOp = ToPlume(state.color_op);
    blend.blendOpAlpha = ToPlume(state.alpha_op);
    blend.renderTargetWriteMask = state.color_write_mask & kWriteAll;
    if (color_attachment) {
        desc->renderTargetBlend[0] = blend;
    }

    // The logic op.
    desc->logicOpEnabled = state.logic_op_enabled;
    desc->logicOp = ToPlume(state.logic_op);

    // The depth test.
    desc->depthEnabled = state.depth_test;
    desc->depthWriteEnabled = state.depth_write;
    desc->depthFunction = ToPlume(state.depth_compare);
    desc->depthClipEnabled = true;

    // Stencil: the operations and the compare function are baked; the reference and the two masks are set
    // on the command list.
    desc->stencilEnabled = state.stencil_test;
    desc->stencilFrontFace.failOp = ToPlume(state.stencil_front.fail);
    desc->stencilFrontFace.passOp = ToPlume(state.stencil_front.pass);
    desc->stencilFrontFace.depthFailOp = ToPlume(state.stencil_front.depth_fail);
    desc->stencilFrontFace.compareFunction = ToPlume(state.stencil_front.compare);
    desc->stencilBackFace.failOp = ToPlume(state.stencil_back.fail);
    desc->stencilBackFace.passOp = ToPlume(state.stencil_back.pass);
    desc->stencilBackFace.depthFailOp = ToPlume(state.stencil_back.depth_fail);
    desc->stencilBackFace.compareFunction = ToPlume(state.stencil_back.compare);

    desc->cullMode = ToPlume(state.cull);
    desc->frontFace = ToPlume(state.front_face);

    // Depth bias: baked. The vendored backend's constant factor is an integer.
    desc->depthBias = 0;
    desc->depthBiasClamp = 0.0f;
    desc->slopeScaledDepthBias = 0.0f;
    if (state.depth_bias_enabled) {
        if (state.depth_bias_constant != std::floor(state.depth_bias_constant) || std::fabs(state.depth_bias_constant) > 2147483000.0f) {
            return Error{fmt::format("depth_bias_constant {} is not a whole number (the vendored backend's constant factor is an integer)",
                                     state.depth_bias_constant)};
        }
        desc->depthBias = static_cast<int32_t>(state.depth_bias_constant);
        desc->depthBiasClamp = state.depth_bias_clamp;
        desc->slopeScaledDepthBias = state.depth_bias_slope;
    }

    // Always dynamic: the blend constants (the vendored queue's patch 07), the stencil reference, compare
    // mask and write mask (patch 10).
    desc->dynamicBlendConstantsEnabled = true;
    desc->dynamicStencilEnabled = true;
    desc->dynamicDepthBiasEnabled = false;
    return Result<void>{};
}

Result<void> ToPlume(const GraphicsPipelineDesc& pipeline, const ::plume::RenderShader* vertex, const ::plume::RenderShader* fragment,
                     const ::plume::RenderShader* geometry, const ::plume::RenderPipelineLayout* layout, PlumeGraphicsDesc* out) {
    ::plume::RenderGraphicsPipelineDesc& desc = out->desc;
    desc = ::plume::RenderGraphicsPipelineDesc();
    desc.pipelineLayout = layout;
    desc.vertexShader = vertex;
    desc.pixelShader = fragment;
    desc.geometryShader = geometry;
    desc.primitiveTopology = ToPlume(pipeline.topology);

    const bool has_color = pipeline.color_format != Format::Undefined;
    desc.renderTargetCount = has_color ? 1 : 0;
    if (has_color) {
        desc.renderTargetFormat[0] = ToPlumeFormat(pipeline.color_format);
    }
    desc.depthTargetFormat = ToPlumeFormat(pipeline.depth_format);

    if (Result<void> state = ToPlume(pipeline.state, has_color, &desc); !state.ok()) {
        return state;
    }

    out->slots.clear();
    out->elements.clear();
    for (const VertexBinding& binding : pipeline.vertex_layout.bindings) {
        out->slots.emplace_back(binding.binding, binding.stride,
                                binding.per_instance ? ::plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                                                     : ::plume::RenderInputSlotClassification::PER_VERTEX_DATA);
    }
    for (const VertexAttribute& attribute : pipeline.vertex_layout.attributes) {
        if (!IsVertexFormat(attribute.format)) {
            return Error{fmt::format("vertex attribute at location {} has a format that is not a vertex format", attribute.location)};
        }
        out->elements.emplace_back("ATTRIBUTE", attribute.location, attribute.location, ToPlumeFormat(attribute.format), attribute.binding,
                                   attribute.offset);
    }
    desc.inputSlots = out->slots.empty() ? nullptr : out->slots.data();
    desc.inputSlotsCount = static_cast<uint32_t>(out->slots.size());
    desc.inputElements = out->elements.empty() ? nullptr : out->elements.data();
    desc.inputElementsCount = static_cast<uint32_t>(out->elements.size());
    return Result<void>{};
}

}  // namespace mh4n::gpu::rhi::core
