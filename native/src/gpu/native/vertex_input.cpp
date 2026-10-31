// Written for this package: nothing is copied from Azahar here.
#include "gpu/native/vertex_input.h"

namespace mh4n::gpu::native {

rhi::Format ToRhiFormat(vertex::VertexFormat format) {
    using vertex::VertexFormat;
    switch (format) {
    case VertexFormat::R8_SINT: return rhi::Format::R8Sint;
    case VertexFormat::R8G8_SINT: return rhi::Format::Rg8Sint;
    case VertexFormat::R8G8B8A8_SINT: return rhi::Format::Rgba8Sint;
    case VertexFormat::R8_UINT: return rhi::Format::R8Uint;
    case VertexFormat::R8G8_UINT: return rhi::Format::Rg8Uint;
    case VertexFormat::R8G8B8A8_UINT: return rhi::Format::Rgba8Uint;
    case VertexFormat::R16_SINT: return rhi::Format::R16Sint;
    case VertexFormat::R16G16_SINT: return rhi::Format::Rg16Sint;
    case VertexFormat::R16G16B16A16_SINT: return rhi::Format::Rgba16Sint;
    case VertexFormat::R32_FLOAT: return rhi::Format::R32Float;
    case VertexFormat::R32G32_FLOAT: return rhi::Format::Rg32Float;
    case VertexFormat::R32G32B32_FLOAT: return rhi::Format::Rgb32Float;
    case VertexFormat::R32G32B32A32_FLOAT: return rhi::Format::Rgba32Float;
    }
    return rhi::Format::Undefined;
}

rhi::Topology ToRhiTopology(vertex::Topology topology) {
    switch (topology) {
    case vertex::Topology::TriangleList: return rhi::Topology::TriangleList;
    case vertex::Topology::TriangleStrip: return rhi::Topology::TriangleStrip;
    case vertex::Topology::TriangleFan: return rhi::Topology::TriangleFan;
    case vertex::Topology::Shader: return rhi::Topology::TriangleList;
    }
    return rhi::Topology::TriangleList;
}

VertexInputKey MakeVertexInput(const vertex::LoadedDraw& draw) {
    VertexInputKey key;
    const size_t bindings = draw.buffers.size() < rhi::kMaxVertexBindings ? draw.buffers.size() : rhi::kMaxVertexBindings;
    key.binding_count = static_cast<u8>(bindings);
    for (size_t i = 0; i < bindings; ++i) {
        key.bindings[i].stride = draw.buffers[i].stride;
        key.bindings[i].per_instance = draw.buffers[i].per_instance ? 1 : 0;
    }
    u32 n = 0;
    for (const vertex::VertexAttribute& attribute : draw.attributes) {
        if (attribute.binding >= bindings) {
            continue;  // no buffer behind it: the register reads the constant buffer's defaults
        }
        VertexInputKey::Attribute& out = key.attributes[n++];
        out.location = static_cast<u8>(attribute.location);
        out.binding = static_cast<u8>(attribute.binding);
        out.format = ToRhiFormat(attribute.format);
        out.offset = attribute.offset;
    }
    key.attribute_count = static_cast<u8>(n);
    return key;
}

} // namespace mh4n::gpu::native
