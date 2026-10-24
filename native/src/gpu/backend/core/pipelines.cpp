// Binding sets and pipelines (N2-26).
#include <fmt/format.h>

#include "gpu/backend/core/impl.h"
#include "gpu/backend/core/to_plume.h"

namespace mh4n::gpu::rhi {

namespace core {

namespace {

::plume::RenderDescriptorRangeType ToRangeType(BindingType type) {
    switch (type) {
    case BindingType::UniformBuffer: return ::plume::RenderDescriptorRangeType::CONSTANT_BUFFER;
    case BindingType::StorageBuffer: return ::plume::RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER;
    case BindingType::TexelBuffer: return ::plume::RenderDescriptorRangeType::FORMATTED_BUFFER;
    case BindingType::SampledImage: return ::plume::RenderDescriptorRangeType::TEXTURE;
    case BindingType::Sampler: return ::plume::RenderDescriptorRangeType::SAMPLER;
    case BindingType::StorageImage: return ::plume::RenderDescriptorRangeType::READ_WRITE_TEXTURE;
    }
    return ::plume::RenderDescriptorRangeType::UNKNOWN;
}

// The ranges of one set (one descriptor per slot, in slot order) and the set's description.
struct SetRanges {
    std::vector<::plume::RenderDescriptorRange> ranges;
};

} // namespace

std::unique_ptr<::plume::RenderPipelineLayout> MakePipelineLayout(Backend::Impl& backend,
                                                                  const std::vector<BindingSetDesc>& sets) {
    std::vector<SetRanges> ranges(sets.size());
    std::vector<::plume::RenderDescriptorSetDesc> descs(sets.size());
    for (size_t i = 0; i < sets.size(); ++i) {
        for (const BindingSlot& slot : sets[i].slots) {
            ranges[i].ranges.emplace_back(ToRangeType(slot.type), slot.binding, 1u);
        }
        descs[i] =
            ::plume::RenderDescriptorSetDesc(ranges[i].ranges.data(), static_cast<uint32_t>(ranges[i].ranges.size()));
    }
    ::plume::RenderPipelineLayoutDesc layout_desc;
    layout_desc.descriptorSetDescs = descs.data();
    layout_desc.descriptorSetDescsCount = static_cast<uint32_t>(descs.size());
    std::unique_ptr<::plume::RenderPipelineLayout> layout = backend.device_owner->createPipelineLayout(layout_desc);
    if (layout != nullptr && static_cast<::plume::VulkanPipelineLayout*>(layout.get())->vk == VK_NULL_HANDLE) {
        return nullptr;
    }
    return layout;
}

Result<std::shared_ptr<BindingSet>> MakeBindingSet(const std::shared_ptr<Backend::Impl>& backend,
                                                   const BindingSetDesc& desc, bool buffers_only) {
    std::vector<::plume::RenderDescriptorRange> ranges;
    for (size_t i = 0; i < desc.slots.size(); ++i) {
        const BindingSlot& slot = desc.slots[i];
        if (buffers_only && slot.type != BindingType::UniformBuffer && slot.type != BindingType::StorageBuffer) {
            return Error{fmt::format("binding {} is not a buffer: binding sets of images, samplers and texel buffers "
                                     "are the targets module's",
                                     slot.binding)};
        }
        for (size_t j = 0; j < i; ++j) {
            if (desc.slots[j].binding == slot.binding) {
                return Error{fmt::format("binding {} appears twice in the set", slot.binding)};
            }
        }
        ranges.emplace_back(ToRangeType(slot.type), slot.binding, 1u);
    }
    if (ranges.empty()) {
        return Error{"a binding set has no slot"};
    }
    auto impl = std::make_unique<BindingSet::Impl>();
    impl->backend = backend;
    impl->desc = desc;
    impl->set = backend->device_owner->createDescriptorSet(
        ::plume::RenderDescriptorSetDesc(ranges.data(), static_cast<uint32_t>(ranges.size())));
    if (impl->set == nullptr || static_cast<::plume::VulkanDescriptorSet*>(impl->set.get())->vk == VK_NULL_HANDLE) {
        return Error{"the binding set could not be made"};
    }
    return std::make_shared<BindingSet>(std::move(impl));
}

} // namespace core

// ---- BindingSet ----------------------------------------------------------------------------------

BindingSet::BindingSet(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BindingSet::~BindingSet() = default;

const BindingSetDesc& BindingSet::Desc() const {
    return impl_->desc;
}

namespace {

Result<void> SetBufferSlot(BindingSet::Impl& set, u32 binding, BindingType type, const Buffer& buffer, u64 offset,
                           u64 size) {
    int slot_index = -1;
    for (size_t i = 0; i < set.desc.slots.size(); ++i) {
        if (set.desc.slots[i].binding == binding) {
            slot_index = static_cast<int>(i);
        }
    }
    if (slot_index < 0) {
        return Error{fmt::format("the set has no binding {}", binding)};
    }
    if (set.desc.slots[slot_index].type != type) {
        return Error{fmt::format("binding {} is not a {} buffer", binding,
                                 type == BindingType::UniformBuffer ? "uniform" : "storage")};
    }
    const BufferUsage needed = type == BindingType::UniformBuffer ? BufferUsage::Uniform : BufferUsage::Storage;
    if (!HasUsage(buffer.Usage(), needed)) {
        return Error{fmt::format("binding {}: the buffer was made without {} usage", binding,
                                 type == BindingType::UniformBuffer ? "Uniform" : "Storage")};
    }
    const Capabilities& caps = set.backend->caps;
    const u64 alignment = type == BindingType::UniformBuffer ? caps.uniform_buffer_offset_alignment
                                                             : caps.storage_buffer_offset_alignment;
    const u64 max_range =
        type == BindingType::UniformBuffer ? caps.max_uniform_buffer_range : caps.max_storage_buffer_range;
    if (alignment > 0 && offset % alignment != 0) {
        return Error{fmt::format("binding {}: offset {} is not a multiple of the device's alignment {}", binding,
                                 offset, alignment)};
    }
    if (size == 0 || offset + size > buffer.Size()) {
        return Error{fmt::format("binding {}: range [{}, {}) is outside the buffer of {} bytes", binding, offset,
                                 offset + size, buffer.Size())};
    }
    if (size > max_range) {
        return Error{fmt::format("binding {}: range {} is above the device's limit {}", binding, size, max_range)};
    }
    if (offset > 0xFFFFFFFFull) {
        return Error{fmt::format("binding {}: offset {} does not fit the binding call (4 GiB)", binding, offset)};
    }
    // The vendored backend binds at an offset through a structured view of stride 1 (rhi.md, "Binding a
    // buffer at an offset").
    const ::plume::RenderBufferStructuredView view(1, static_cast<uint32_t>(offset));
    set.set->setBuffer(static_cast<uint32_t>(slot_index), buffer.impl().buffer.get(), size, &view);
    return Result<void>{};
}

} // namespace

Result<void> BindingSet::SetUniformBuffer(u32 binding, const Buffer& buffer, u64 offset, u64 size) {
    return SetBufferSlot(*impl_, binding, BindingType::UniformBuffer, buffer, offset, size);
}

Result<void> BindingSet::SetStorageBuffer(u32 binding, const Buffer& buffer, u64 offset, u64 size) {
    return SetBufferSlot(*impl_, binding, BindingType::StorageBuffer, buffer, offset, size);
}

Result<std::shared_ptr<BindingSet>> Backend::CreateBindingSet(const BindingSetDesc& desc) {
    return core::MakeBindingSet(impl_, desc, true);
}

// ---- Pipelines -----------------------------------------------------------------------------------

Pipeline::Pipeline(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Pipeline::~Pipeline() = default;

bool Pipeline::IsCompute() const {
    return impl_->compute;
}

namespace {

std::unique_ptr<::plume::RenderShader> MakeShader(Backend::Impl& backend, const ShaderCode& code, const char* what,
                                                  std::string* error) {
    if (code.spirv.empty() || code.spirv[0] != 0x07230203u) {
        *error = fmt::format("the {} shader is not SPIR-V (empty, or no magic number)", what);
        return nullptr;
    }
    std::unique_ptr<::plume::RenderShader> shader = backend.device_owner->createShader(
        code.spirv.data(), code.spirv.size_bytes(), code.entry.c_str(), ::plume::RenderShaderFormat::SPIRV);
    if (shader == nullptr || static_cast<::plume::VulkanShader*>(shader.get())->vk == VK_NULL_HANDLE) {
        *error = fmt::format("the {} shader module could not be made", what);
        return nullptr;
    }
    return shader;
}

} // namespace

Result<std::shared_ptr<Pipeline>> Backend::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    Backend::Impl& backend = *impl_;
    const Capabilities& caps = backend.caps;
    const std::string where = fmt::format("pipeline \"{}\": ", desc.name);

    if (desc.topology == Topology::TriangleFan && !caps.triangle_fans) {
        return Error{where + "triangle fans are not available on this device (or masked): rewrite them as lists"};
    }
    if (desc.state.logic_op_enabled && !caps.logic_op) {
        return Error{where +
                     "hardware logic ops are not available on this device (or masked): emulate them in the shader"};
    }
    if (desc.state.logic_op_enabled && desc.state.blend_enabled) {
        return Error{where + "a logic op and blending are exclusive"};
    }
    if (!desc.geometry.spirv.empty() && !caps.geometry_shader) {
        return Error{where + "geometry shaders are not available on this device"};
    }
    if (desc.vertex_layout.bindings.size() > kMaxVertexBindings ||
        desc.vertex_layout.attributes.size() > kMaxVertexAttributes) {
        return Error{where + fmt::format("{} vertex bindings and {} attributes: the limits are {} and {}",
                                         desc.vertex_layout.bindings.size(), desc.vertex_layout.attributes.size(),
                                         kMaxVertexBindings, kMaxVertexAttributes)};
    }
    for (const VertexBinding& binding : desc.vertex_layout.bindings) {
        if (binding.binding >= kMaxVertexBindings) {
            return Error{where + fmt::format("vertex binding {} is above the limit", binding.binding)};
        }
        if (caps.stride_alignment > 1 && binding.stride % caps.stride_alignment != 0) {
            return Error{where + fmt::format("the stride {} of vertex binding {} is not a multiple of the device's {}",
                                             binding.stride, binding.binding, caps.stride_alignment)};
        }
    }
    for (const VertexAttribute& attribute : desc.vertex_layout.attributes) {
        bool declared = false;
        for (const VertexBinding& binding : desc.vertex_layout.bindings) {
            declared = declared || binding.binding == attribute.binding;
        }
        if (!declared) {
            return Error{where + fmt::format("attribute {} uses vertex binding {}, which the layout does not declare",
                                             attribute.location, attribute.binding)};
        }
    }
    if (desc.color_format != Format::Undefined && core::IsDepthFormat(desc.color_format)) {
        return Error{where + "the colour format is a depth format"};
    }
    if (desc.depth_format != Format::Undefined && !core::IsDepthFormat(desc.depth_format)) {
        return Error{where + "the depth format is not a depth format"};
    }

    std::string failure;
    std::unique_ptr<::plume::RenderShader> vertex = MakeShader(backend, desc.vertex, "vertex", &failure);
    if (vertex == nullptr) {
        return Error{where + failure};
    }
    std::unique_ptr<::plume::RenderShader> fragment;
    if (!desc.fragment.spirv.empty()) {
        fragment = MakeShader(backend, desc.fragment, "fragment", &failure);
        if (fragment == nullptr) {
            return Error{where + failure};
        }
    }
    std::unique_ptr<::plume::RenderShader> geometry;
    if (!desc.geometry.spirv.empty()) {
        geometry = MakeShader(backend, desc.geometry, "geometry", &failure);
        if (geometry == nullptr) {
            return Error{where + failure};
        }
    }

    auto impl = std::make_unique<Pipeline::Impl>();
    impl->backend = impl_->shared_from_this();
    impl->sets = desc.sets;
    impl->layout = core::MakePipelineLayout(backend, desc.sets);
    if (impl->layout == nullptr) {
        return Error{where + "the pipeline layout could not be made"};
    }

    core::PlumeGraphicsDesc plume_desc;
    if (Result<void> translated =
            core::ToPlume(desc, vertex.get(), fragment.get(), geometry.get(), impl->layout.get(), &plume_desc);
        !translated.ok()) {
        return Error{where + translated.error()};
    }
    impl->pipeline = backend.device_owner->createGraphicsPipeline(plume_desc.desc);
    if (impl->pipeline == nullptr ||
        static_cast<::plume::VulkanGraphicsPipeline*>(impl->pipeline.get())->vk == VK_NULL_HANDLE) {
        return Error{where + "the graphics pipeline could not be made"};
    }
    if (!desc.name.empty()) {
        impl->pipeline->setName(desc.name);
    }
    return std::make_shared<Pipeline>(std::move(impl));
}

Result<std::shared_ptr<Pipeline>> Backend::CreateComputePipeline(const ComputePipelineDesc& desc) {
    Backend::Impl& backend = *impl_;
    const std::string where = fmt::format("compute pipeline \"{}\": ", desc.name);
    std::string failure;
    std::unique_ptr<::plume::RenderShader> shader = MakeShader(backend, desc.shader, "compute", &failure);
    if (shader == nullptr) {
        return Error{where + failure};
    }
    auto impl = std::make_unique<Pipeline::Impl>();
    impl->backend = impl_->shared_from_this();
    impl->compute = true;
    impl->sets = desc.sets;
    impl->layout = core::MakePipelineLayout(backend, desc.sets);
    if (impl->layout == nullptr) {
        return Error{where + "the pipeline layout could not be made"};
    }
    // The thread group size is in the SPIR-V; the vendored backend only wants it non-zero.
    ::plume::RenderComputePipelineDesc plume_desc(impl->layout.get(), shader.get(), 1, 1, 1);
    impl->pipeline = backend.device_owner->createComputePipeline(plume_desc);
    if (impl->pipeline == nullptr ||
        static_cast<::plume::VulkanComputePipeline*>(impl->pipeline.get())->vk == VK_NULL_HANDLE) {
        return Error{where + "the compute pipeline could not be made"};
    }
    if (!desc.name.empty()) {
        impl->pipeline->setName(desc.name);
    }
    return std::make_shared<Pipeline>(std::move(impl));
}

} // namespace mh4n::gpu::rhi
