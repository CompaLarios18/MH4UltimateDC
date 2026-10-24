// The Vulkan side (vulkan_access.h) that the core defines (N2-26). NativeImage and WrapExternalImage are
// the targets module's.
#include "gpu/backend/core/impl.h"

namespace mh4n::gpu::rhi {

VulkanHandles Handles(const Backend& backend) {
    const Backend::Impl& impl = backend.impl();
    VulkanHandles handles;
    handles.instance = impl.instance->impl().vulkan()->instance;
    handles.physical_device = impl.physical_device;
    handles.device = impl.device->vk;
    handles.queue_family_index = impl.queue_family;
    handles.queue = impl.vk_queue;
    return handles;
}

VkInstance NativeInstance(const Instance& instance) {
    return instance.impl().vulkan()->instance;
}

VkCommandBuffer CommandBuffer(Recorder& recorder) {
    Recorder::Impl& impl = recorder.impl();
    if (impl.in_pass) {
        impl.vulkan_list()->checkActiveRenderPass();  // begins the pass the first draw would begin
    }
    return impl.vulkan_list()->vk;
}

SurfaceHandle SurfaceFrom(VkSurfaceKHR surface) {
    SurfaceHandle handle;
    handle.value = reinterpret_cast<u64>(surface);
    return handle;
}

VkSurfaceKHR VulkanSurface(SurfaceHandle surface) {
    return reinterpret_cast<VkSurfaceKHR>(surface.value);
}

QueueLock::QueueLock(Backend& backend) : lock_(*backend.impl().queue_mutex) {}
QueueLock::~QueueLock() = default;

}  // namespace mh4n::gpu::rhi
