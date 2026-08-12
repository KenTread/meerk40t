// SPDX-License-Identifier: LicenseRef-OmniFractal-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// Vulkan 1.3 compute context. Headless: no surface, no swapchain, no
// graphics queue required -- the engine only ever dispatches compute and
// reads results back through a host-visible staging buffer.

#pragma once

#include "omf/Internal.hpp"

#include <functional>
#include <optional>

#if defined(OMF_WITH_VULKAN)
#  include <volk.h>
#  include <vk_mem_alloc.h>
#endif

namespace omf {

#if defined(OMF_WITH_VULKAN)

// RAII wrapper for a device-local or host-visible buffer allocated
// through VMA.
class GpuBuffer {
public:
    GpuBuffer() = default;
    GpuBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
              VmaMemoryUsage memory_usage);
    ~GpuBuffer();

    GpuBuffer(GpuBuffer&& other) noexcept;
    GpuBuffer& operator=(GpuBuffer&& other) noexcept;
    GpuBuffer(const GpuBuffer&) = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;

    VkBuffer handle() const { return buffer_; }
    VkDeviceSize size() const { return size_; }
    void* mapped() const { return mapped_; }

    void upload(const void* data, std::size_t bytes, std::size_t offset = 0);
    void download(void* data, std::size_t bytes, std::size_t offset = 0) const;

private:
    void reset();

    VmaAllocator  allocator_{VK_NULL_HANDLE};
    VkBuffer      buffer_{VK_NULL_HANDLE};
    VmaAllocation allocation_{VK_NULL_HANDLE};
    VkDeviceSize  size_{0};
    void*         mapped_{nullptr};
};

// A compute pipeline plus the descriptor plumbing it needs. Every shader
// in this engine uses the same descriptor shape -- N storage buffers and
// a push-constant block -- which keeps this class small.
class ComputePipeline {
public:
    ComputePipeline() = default;
    ComputePipeline(VkDevice device, std::span<const std::uint32_t> spirv,
                    std::uint32_t buffer_count, std::uint32_t push_constant_bytes);
    ~ComputePipeline();

    ComputePipeline(ComputePipeline&&) noexcept;
    ComputePipeline& operator=(ComputePipeline&&) noexcept;
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    VkPipeline handle() const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }
    VkDescriptorSetLayout set_layout() const { return set_layout_; }

private:
    void reset();

    VkDevice              device_{VK_NULL_HANDLE};
    VkShaderModule        module_{VK_NULL_HANDLE};
    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout      layout_{VK_NULL_HANDLE};
    VkPipeline            pipeline_{VK_NULL_HANDLE};
};

#endif  // OMF_WITH_VULKAN

// Front door. Constructing this never throws on "no GPU"; call
// available() and fall back to the CPU path when it returns false.
class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // True when a compute-capable device was found and initialised.
    bool available() const { return available_; }
    const std::string& device_name() const { return device_name_; }
    const std::string& init_error() const { return init_error_; }

#if defined(OMF_WITH_VULKAN)
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    std::uint32_t queue_family() const { return queue_family_; }
    VmaAllocator allocator() const { return allocator_; }

    // Records `record` into a one-shot command buffer, submits it and
    // waits for the fence. Compute work here is measured in seconds, so
    // the simplicity of a blocking submit beats pipelining.
    void submit_blocking(const std::function<void(VkCommandBuffer)>& record);

    // Allocates and writes a descriptor set binding `buffers` at
    // bindings 0..n-1.
    VkDescriptorSet allocate_set(VkDescriptorSetLayout layout,
                                 std::span<const VkBuffer> buffers,
                                 std::span<const VkDeviceSize> sizes);
#endif

private:
    bool        available_{false};
    std::string device_name_{"CPU"};
    std::string init_error_;

#if defined(OMF_WITH_VULKAN)
    VkInstance       instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_{VK_NULL_HANDLE};
    VkDevice         device_{VK_NULL_HANDLE};
    VkQueue          queue_{VK_NULL_HANDLE};
    std::uint32_t    queue_family_{0};
    VkCommandPool    command_pool_{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    VmaAllocator     allocator_{VK_NULL_HANDLE};
#endif
};

// Loads a compiled SPIR-V module shipped next to the library. Returns
// nullopt when the file is missing, which is not fatal -- the caller
// drops to the CPU path.
std::optional<std::vector<std::uint32_t>> load_spirv(std::string_view name);

}  // namespace omf
