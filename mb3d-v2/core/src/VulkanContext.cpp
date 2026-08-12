// SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// VulkanContext.cpp -- headless Vulkan 1.3 compute bring-up.
//
// Design notes:
//   * volk is used for loading so the library has no link-time
//     dependency on a Vulkan loader; a machine with no ICD simply fails
//     volkInitialize() and we fall back to the CPU path.
//   * Device selection prefers a discrete GPU with a dedicated compute
//     queue family. An async-compute-only queue lets long fractal
//     dispatches run without starving the desktop compositor, which
//     matters because these dispatches take seconds, not milliseconds.
//   * Nothing here throws on absence of hardware -- only on a device
//     that exists but then misbehaves.

#include "mb3d/VulkanContext.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#if defined(MB3D_WITH_VULKAN)
#  define VMA_IMPLEMENTATION
#  define VMA_STATIC_VULKAN_FUNCTIONS 0
#  define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#  include <vk_mem_alloc.h>
#endif

namespace mb3d {

// ---------------------------------------------------------------------
// SPIR-V loading
// ---------------------------------------------------------------------

namespace {

// Shaders are installed alongside the shared library. Look there first,
// then in a MB3D_SHADER_DIR override, then in the build tree layout.
std::vector<std::filesystem::path> shader_search_paths(std::string_view name) {
    std::vector<std::filesystem::path> paths;
    if (const char* env = std::getenv("MB3D_SHADER_DIR")) {
        paths.emplace_back(std::filesystem::path(env) / name);
    }
    paths.emplace_back(std::filesystem::path("shaders") / name);
    paths.emplace_back(std::filesystem::path("core") / "shaders" / name);
    paths.emplace_back(std::filesystem::path(MB3D_INSTALL_SHADER_DIR) / name);
    return paths;
}

}  // namespace

std::optional<std::vector<std::uint32_t>> load_spirv(std::string_view name) {
    for (const auto& path : shader_search_paths(name)) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) continue;

        const auto size = static_cast<std::size_t>(file.tellg());
        if (size == 0 || (size % 4) != 0) continue;

        std::vector<std::uint32_t> words(size / 4);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size));
        if (!file) continue;

        // SPIR-V magic number; guards against a stale GLSL file.
        if (words.empty() || words[0] != 0x07230203u) continue;
        return words;
    }
    return std::nullopt;
}

#if !defined(MB3D_WITH_VULKAN)

// ---------------------------------------------------------------------
// Build without Vulkan: everything reports unavailable and the engine
// runs entirely on CpuRaymarcher.
// ---------------------------------------------------------------------

VulkanContext::VulkanContext() {
    available_ = false;
    init_error_ = "built without Vulkan support (MB3D_WITH_VULKAN=OFF)";
}

VulkanContext::~VulkanContext() = default;

#else

// ---------------------------------------------------------------------
// GpuBuffer
// ---------------------------------------------------------------------

GpuBuffer::GpuBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                     VmaMemoryUsage memory_usage)
    : allocator_(allocator), size_(size) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = memory_usage;
    if (memory_usage == VMA_MEMORY_USAGE_CPU_TO_GPU ||
        memory_usage == VMA_MEMORY_USAGE_GPU_TO_CPU) {
        alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    }

    VmaAllocationInfo alloc_info{};
    const VkResult r = vmaCreateBuffer(allocator_, &info, &alloc, &buffer_, &allocation_,
                                       &alloc_info);
    require(r == VK_SUCCESS, MB3D_ERR_OUT_OF_MEMORY, "vmaCreateBuffer failed");
    mapped_ = alloc_info.pMappedData;
}

void GpuBuffer::reset() {
    if (allocator_ && buffer_) vmaDestroyBuffer(allocator_, buffer_, allocation_);
    allocator_ = VK_NULL_HANDLE;
    buffer_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    mapped_ = nullptr;
    size_ = 0;
}

GpuBuffer::~GpuBuffer() { reset(); }

GpuBuffer::GpuBuffer(GpuBuffer&& other) noexcept
    : allocator_(other.allocator_), buffer_(other.buffer_), allocation_(other.allocation_),
      size_(other.size_), mapped_(other.mapped_) {
    other.allocator_ = VK_NULL_HANDLE;
    other.buffer_ = VK_NULL_HANDLE;
    other.allocation_ = VK_NULL_HANDLE;
    other.size_ = 0;
    other.mapped_ = nullptr;
}

GpuBuffer& GpuBuffer::operator=(GpuBuffer&& other) noexcept {
    if (this != &other) {
        reset();
        allocator_ = other.allocator_;
        buffer_ = other.buffer_;
        allocation_ = other.allocation_;
        size_ = other.size_;
        mapped_ = other.mapped_;
        other.allocator_ = VK_NULL_HANDLE;
        other.buffer_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.size_ = 0;
        other.mapped_ = nullptr;
    }
    return *this;
}

void GpuBuffer::upload(const void* data, std::size_t bytes, std::size_t offset) {
    require(mapped_ != nullptr, MB3D_ERR_INVALID_ARG, "upload to unmapped buffer");
    require(offset + bytes <= size_, MB3D_ERR_INVALID_ARG, "upload overruns buffer");
    std::memcpy(static_cast<std::byte*>(mapped_) + offset, data, bytes);
    vmaFlushAllocation(allocator_, allocation_, offset, bytes);
}

void GpuBuffer::download(void* data, std::size_t bytes, std::size_t offset) const {
    require(mapped_ != nullptr, MB3D_ERR_INVALID_ARG, "download from unmapped buffer");
    require(offset + bytes <= size_, MB3D_ERR_INVALID_ARG, "download overruns buffer");
    vmaInvalidateAllocation(allocator_, allocation_, offset, bytes);
    std::memcpy(data, static_cast<const std::byte*>(mapped_) + offset, bytes);
}

// ---------------------------------------------------------------------
// ComputePipeline
// ---------------------------------------------------------------------

ComputePipeline::ComputePipeline(VkDevice device, std::span<const std::uint32_t> spirv,
                                 std::uint32_t buffer_count,
                                 std::uint32_t push_constant_bytes)
    : device_(device) {
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = spirv.size() * sizeof(std::uint32_t);
    module_info.pCode = spirv.data();
    require(vkCreateShaderModule(device_, &module_info, nullptr, &module_) == VK_SUCCESS,
            MB3D_ERR_SHADER_COMPILE, "vkCreateShaderModule failed");

    std::vector<VkDescriptorSetLayoutBinding> bindings(buffer_count);
    for (std::uint32_t i = 0; i < buffer_count; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo set_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    set_info.pBindings = bindings.data();
    require(vkCreateDescriptorSetLayout(device_, &set_info, nullptr, &set_layout_) == VK_SUCCESS,
            MB3D_ERR_UNKNOWN, "vkCreateDescriptorSetLayout failed");

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.offset = 0;
    push.size = push_constant_bytes;

    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout_;
    layout_info.pushConstantRangeCount = push_constant_bytes > 0 ? 1 : 0;
    layout_info.pPushConstantRanges = push_constant_bytes > 0 ? &push : nullptr;
    require(vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) == VK_SUCCESS,
            MB3D_ERR_UNKNOWN, "vkCreatePipelineLayout failed");

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module_;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage = stage;
    pipeline_info.layout = layout_;
    require(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                     &pipeline_) == VK_SUCCESS,
            MB3D_ERR_SHADER_COMPILE, "vkCreateComputePipelines failed");
}

void ComputePipeline::reset() {
    if (device_ == VK_NULL_HANDLE) return;
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (module_) vkDestroyShaderModule(device_, module_, nullptr);
    device_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    module_ = VK_NULL_HANDLE;
}

ComputePipeline::~ComputePipeline() { reset(); }

ComputePipeline::ComputePipeline(ComputePipeline&& o) noexcept
    : device_(o.device_), module_(o.module_), set_layout_(o.set_layout_),
      layout_(o.layout_), pipeline_(o.pipeline_) {
    o.device_ = VK_NULL_HANDLE;
    o.module_ = VK_NULL_HANDLE;
    o.set_layout_ = VK_NULL_HANDLE;
    o.layout_ = VK_NULL_HANDLE;
    o.pipeline_ = VK_NULL_HANDLE;
}

ComputePipeline& ComputePipeline::operator=(ComputePipeline&& o) noexcept {
    if (this != &o) {
        reset();
        device_ = o.device_; module_ = o.module_; set_layout_ = o.set_layout_;
        layout_ = o.layout_; pipeline_ = o.pipeline_;
        o.device_ = VK_NULL_HANDLE; o.module_ = VK_NULL_HANDLE;
        o.set_layout_ = VK_NULL_HANDLE; o.layout_ = VK_NULL_HANDLE;
        o.pipeline_ = VK_NULL_HANDLE;
    }
    return *this;
}

// ---------------------------------------------------------------------
// VulkanContext
// ---------------------------------------------------------------------

namespace {

struct QueuePick {
    std::uint32_t family{0};
    bool found{false};
    bool dedicated_compute{false};
};

QueuePick pick_compute_queue(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    QueuePick pick;
    // Prefer a compute family without graphics -- async compute keeps a
    // multi-second dispatch from stalling the desktop.
    for (std::uint32_t i = 0; i < count; ++i) {
        const bool compute = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        const bool graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        if (compute && !graphics) return {i, true, true};
        if (compute && !pick.found) pick = {i, true, false};
    }
    return pick;
}

int score_device(VkPhysicalDevice device) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device, &props);
    switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 1000;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 500;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 250;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 100;
        default:                                     return 10;
    }
}

}  // namespace

VulkanContext::VulkanContext() {
    if (volkInitialize() != VK_SUCCESS) {
        init_error_ = "no Vulkan loader present";
        return;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "MB3D_Core";
    app.applicationVersion = VK_MAKE_VERSION(MB3D_VERSION_MAJOR, MB3D_VERSION_MINOR,
                                             MB3D_VERSION_PATCH);
    app.pEngineName = "MB3D";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;

    if (vkCreateInstance(&instance_info, nullptr, &instance_) != VK_SUCCESS) {
        init_error_ = "vkCreateInstance failed";
        return;
    }
    volkLoadInstance(instance_);

    std::uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (device_count == 0) {
        init_error_ = "no Vulkan physical devices";
        return;
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());

    int best_score = -1;
    QueuePick best_queue;
    for (VkPhysicalDevice candidate : devices) {
        const QueuePick pick = pick_compute_queue(candidate);
        if (!pick.found) continue;
        const int score = score_device(candidate) + (pick.dedicated_compute ? 50 : 0);
        if (score > best_score) {
            best_score = score;
            physical_ = candidate;
            best_queue = pick;
        }
    }
    if (physical_ == VK_NULL_HANDLE) {
        init_error_ = "no compute-capable Vulkan device";
        return;
    }
    queue_family_ = best_queue.family;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_, &props);
    device_name_ = props.deviceName;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    // Vulkan 1.3 core features the shaders rely on.
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE;
    f12.shaderFloat16 = VK_FALSE;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.pNext = &f12;
    f13.maintenance4 = VK_TRUE;
    f13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f13;
    features.features.shaderFloat64 = VK_TRUE;   // deep zooms need doubles
    features.features.shaderInt64 = VK_TRUE;

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;

    if (vkCreateDevice(physical_, &device_info, nullptr, &device_) != VK_SUCCESS) {
        // Retry without float64: plenty of integrated parts lack it, and
        // single precision is fine at shallow zoom levels.
        features.features.shaderFloat64 = VK_FALSE;
        if (vkCreateDevice(physical_, &device_info, nullptr, &device_) != VK_SUCCESS) {
            init_error_ = "vkCreateDevice failed";
            return;
        }
    }
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VmaVulkanFunctions vma_fns{};
    vma_fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vma_fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo alloc_info{};
    alloc_info.physicalDevice = physical_;
    alloc_info.device = device_;
    alloc_info.instance = instance_;
    alloc_info.vulkanApiVersion = VK_API_VERSION_1_3;
    alloc_info.pVulkanFunctions = &vma_fns;
    if (vmaCreateAllocator(&alloc_info, &allocator_) != VK_SUCCESS) {
        init_error_ = "vmaCreateAllocator failed";
        return;
    }

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_;
    if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
        init_error_ = "vkCreateCommandPool failed";
        return;
    }

    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256}};
    VkDescriptorPoolCreateInfo desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    desc_info.maxSets = 64;
    desc_info.poolSizeCount = 1;
    desc_info.pPoolSizes = sizes;
    desc_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    if (vkCreateDescriptorPool(device_, &desc_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        init_error_ = "vkCreateDescriptorPool failed";
        return;
    }

    available_ = true;
}

VulkanContext::~VulkanContext() {
    if (device_) vkDeviceWaitIdle(device_);
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
    if (allocator_) vmaDestroyAllocator(allocator_);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

void VulkanContext::submit_blocking(const std::function<void(VkCommandBuffer)>& record) {
    require(available_, MB3D_ERR_NO_DEVICE, "Vulkan context unavailable");

    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = command_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    require(vkAllocateCommandBuffers(device_, &alloc, &cmd) == VK_SUCCESS,
            MB3D_ERR_UNKNOWN, "vkAllocateCommandBuffers failed");

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    record(cmd);
    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device_, &fence_info, nullptr, &fence);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &submit, fence);

    // Generous timeout: a 2048^3 marching-cubes dispatch is not fast.
    constexpr std::uint64_t kTimeoutNs = 300ull * 1000 * 1000 * 1000;
    const VkResult wait = vkWaitForFences(device_, 1, &fence, VK_TRUE, kTimeoutNs);

    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    require(wait == VK_SUCCESS, MB3D_ERR_UNKNOWN, "compute dispatch timed out");
}

VkDescriptorSet VulkanContext::allocate_set(VkDescriptorSetLayout layout,
                                            std::span<const VkBuffer> buffers,
                                            std::span<const VkDeviceSize> sizes) {
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptor_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    require(vkAllocateDescriptorSets(device_, &alloc, &set) == VK_SUCCESS,
            MB3D_ERR_OUT_OF_MEMORY, "vkAllocateDescriptorSets failed");

    std::vector<VkDescriptorBufferInfo> infos(buffers.size());
    std::vector<VkWriteDescriptorSet> writes(buffers.size());
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        infos[i].buffer = buffers[i];
        infos[i].offset = 0;
        infos[i].range = sizes[i];

        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set;
        writes[i].dstBinding = static_cast<std::uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
    return set;
}

#endif  // MB3D_WITH_VULKAN

}  // namespace mb3d
