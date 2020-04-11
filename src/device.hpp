// Copyright 2018 The clvk authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <spirv/1.0/spirv.hpp>
#include <vulkan/vulkan.h>

#include "cl_headers.hpp"
#include "icd.hpp"
#include "vkutils.hpp"

#define MAKE_NAME_VERSION(major, minor, patch, name)                           \
    cl_name_version_khr { CL_MAKE_VERSION_KHR(major, minor, patch), name }

static cl_version_khr gOpenCLVersion = CL_MAKE_VERSION_KHR(1, 2, 0);

static constexpr bool devices_support_images() { return true; }

struct cvk_device : public _cl_device_id {

    cvk_device(VkPhysicalDevice pd) : m_pdev(pd) {
        vkGetPhysicalDeviceProperties(m_pdev, &m_properties);
        vkGetPhysicalDeviceMemoryProperties(m_pdev, &m_mem_properties);
    }

    static cvk_device* create(VkPhysicalDevice pdev);

    virtual ~cvk_device() { vkDestroyDevice(m_dev, nullptr); }

    const VkPhysicalDeviceLimits& vulkan_limits() const {
        return m_properties.limits;
    }
    const char* name() const { return m_properties.deviceName; }
    uint32_t vendor_id() const { return m_properties.vendorID; }

    CHECK_RETURN uint32_t memory_type_index_for_resource(
        uint32_t valid_memory_type_bits,
        VkMemoryPropertyFlags required_properties) const {

        for (uint32_t k = 0; k < m_mem_properties.memoryTypeCount; k++) {
            auto dev_properties = m_mem_properties.memoryTypes[k].propertyFlags;
            bool valid = (1ULL << k) & valid_memory_type_bits;
            bool satisfactory =
                (dev_properties & required_properties) == required_properties;
            if (satisfactory && valid) {
                return k;
            }
        }

        return VK_MAX_MEMORY_TYPES;
    }

    CHECK_RETURN uint32_t memory_type_index_for_resource(
        uint32_t valid_memory_type_bits, int num_supported,
        uint32_t* supported_memory_types) const {

        for (int i = 0; i < num_supported; i++) {
            auto k = memory_type_index_for_resource(valid_memory_type_bits,
                                                    supported_memory_types[i]);
            if (k != VK_MAX_MEMORY_TYPES) {
                cvk_debug_fn("outer returning %u\n", k);
                return k;
            }
        }

        return VK_MAX_MEMORY_TYPES;
    }

    struct allocation_parameters {
        VkDeviceSize size;
        uint32_t memory_type_index;
    };

    CHECK_RETURN allocation_parameters select_memory_for(VkImage image) const {
        VkMemoryRequirements memreqs;
        vkGetImageMemoryRequirements(m_dev, image, &memreqs);

        allocation_parameters ret;
        ret.size = memreqs.size;
        ret.memory_type_index = memory_type_index_for_resource(
            memreqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        return ret;
    }

    CHECK_RETURN allocation_parameters
    select_memory_for(VkBuffer buffer, cl_mem_flags flags) const {
        UNUSED(flags);
        VkMemoryRequirements memreqs;
        vkGetBufferMemoryRequirements(m_dev, buffer, &memreqs);

        uint32_t supported_memory_types[] = {
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,

            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        };

        allocation_parameters ret;
        ret.size = memreqs.size;
        ret.memory_type_index = memory_type_index_for_resource(
            memreqs.memoryTypeBits, ARRAY_SIZE(supported_memory_types),
            supported_memory_types);

        return ret;
    }

    uint64_t actual_memory_size() const {
        // Be conservative for now and return the size of the smallest memory heap
        uint64_t size = UINT64_MAX;
        for (uint32_t i = 0; i < m_mem_properties.memoryHeapCount; i++) {
            size = std::min(size, m_mem_properties.memoryHeaps[i].size);
        }
        return size;
    }

    uint64_t memory_size() const {
        return std::min(max_alloc_size() * 4, actual_memory_size());
    }

    uint64_t max_alloc_size() const {
        uint64_t max_buffer_size = m_properties.limits.maxStorageBufferRange;
        return std::min(max_buffer_size, actual_memory_size());
    }

    cl_uint mem_base_addr_align() const { return m_mem_base_addr_align; }

    cl_uint max_samplers() const {
        // There are only 20 different possible samplers in OpenCL 1.2, cap the
        // number of supported samplers to that to help with negative testing of
        // the limit against Vulkan implementations that report a very large
        // number for maxPerStageDescriptorSamplers.
        return std::min(20u, vulkan_limits().maxPerStageDescriptorSamplers);
    }

    cl_uint max_work_item_dimensions() const { return 3; }

    bool supports_images() const {
        return devices_support_images() ? CL_TRUE : CL_FALSE;
    }

    CHECK_RETURN const std::string& extension_string() const {
        return m_extension_string;
    }
    CHECK_RETURN const std::vector<cl_name_version_khr>& extensions() const {
        return m_extensions;
    }

    /// Returns true if the device supports the given SPIR-V capability.
    CHECK_RETURN bool supports_capability(spv::Capability capability) const;

    /// Returns true if std430 layout is supported for uniform buffers.
    CHECK_RETURN bool supports_ubo_stdlayout() const {
        return m_features_ubo_stdlayout.uniformBufferStandardLayout;
    }

    cl_version_khr version() const { return gOpenCLVersion; }

    cl_version_khr c_version() const { return gOpenCLVersion; }

    std::string version_string() const {
        return "OpenCL " + std::to_string(CL_VERSION_MAJOR_KHR(version())) +
               "." + std::to_string(CL_VERSION_MINOR_KHR(version())) + " " +
               version_desc();
    }

    std::string c_version_string() const {
        return "OpenCL C " + std::to_string(CL_VERSION_MAJOR_KHR(c_version())) +
               "." + std::to_string(CL_VERSION_MINOR_KHR(c_version())) + " " +
               version_desc();
    }

    std::string profile() const { return "FULL_PROFILE"; }

    std::string driver_version() const {
        return std::to_string(CL_VERSION_MAJOR_KHR(version())) + "." +
               std::to_string(CL_VERSION_MINOR_KHR(version())) + " " +
               version_desc();
    }

    const std::string& ils_string() const { return m_ils_string; }

    const std::vector<cl_name_version_khr>& ils() const { return m_ils; }

    cl_device_type type() const {
        cl_device_type ret;

        switch (m_properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            ret = CL_DEVICE_TYPE_GPU;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            ret = CL_DEVICE_TYPE_CPU;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        default:
            ret = CL_DEVICE_TYPE_CUSTOM;
            break;
        }

        return ret;
    }

    cl_bool has_host_unified_memory() const {
        switch (m_properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return CL_TRUE;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        default:
            return CL_FALSE;
        }
    }

    cvk_vulkan_queue_wrapper& vulkan_queue_allocate() {
        // Simple round-robin allocation for now

        auto& queue = m_vulkan_queues[m_vulkan_queue_alloc_index++];

        if (m_vulkan_queue_alloc_index == m_vulkan_queues.size()) {
            m_vulkan_queue_alloc_index = 0;
        }

        return queue;
    }

    cl_device_fp_config fp_config(cl_device_info fptype) const {
        if (fptype == CL_DEVICE_SINGLE_FP_CONFIG) {
            return CL_FP_ROUND_TO_NEAREST | CL_FP_INF_NAN;
        }

        if ((fptype == CL_DEVICE_DOUBLE_FP_CONFIG) &&
            m_features.features.shaderFloat64) {
            return CL_FP_FMA | CL_FP_ROUND_TO_NEAREST | CL_FP_ROUND_TO_ZERO |
                   CL_FP_ROUND_TO_INF | CL_FP_INF_NAN | CL_FP_DENORM;
        }

        return 0;
    }

    VkPhysicalDevice vulkan_physical_device() const { return m_pdev; }

    VkDevice vulkan_device() const { return m_dev; }

    uint32_t vulkan_max_push_constants_size() const {
        return m_properties.limits.maxPushConstantsSize;
    }

private:
    std::string version_desc() const {
        std::string ret = "CLVK on Vulkan v";
        ret += vulkan_version_string(m_properties.apiVersion);
        ret += " driver " + std::to_string(m_properties.driverVersion);
        return ret;
    }

    CHECK_RETURN bool init_queues(uint32_t* num_queues, uint32_t* queue_family);
    CHECK_RETURN bool init_extensions();
    void init_features();
    void build_extension_ils_list();
    CHECK_RETURN bool create_vulkan_queues_and_device(uint32_t num_queues,
                                                      uint32_t queue_family);
    CHECK_RETURN bool compute_buffer_alignement_requirements();
    void log_limits_and_memory_information();
    CHECK_RETURN bool init();

    VkPhysicalDevice m_pdev;
    VkPhysicalDeviceProperties m_properties;
    VkPhysicalDeviceMemoryProperties m_mem_properties;
    // Vulkan features
    VkPhysicalDeviceFeatures2 m_features{};
    VkPhysicalDeviceVariablePointerFeatures m_features_variable_pointer{};
    VkPhysicalDeviceShaderFloat16Int8FeaturesKHR m_features_float16_int8{};
    VkPhysicalDeviceUniformBufferStandardLayoutFeaturesKHR
        m_features_ubo_stdlayout{};

    VkDevice m_dev;
    std::vector<const char*> m_vulkan_device_extensions;
    cl_uint m_mem_base_addr_align;

    std::vector<cvk_vulkan_queue_wrapper> m_vulkan_queues;
    uint32_t m_vulkan_queue_alloc_index;

    std::string m_extension_string;
    std::vector<cl_name_version_khr> m_extensions;
    std::string m_ils_string;
    std::vector<cl_name_version_khr> m_ils;
};

static inline cvk_device* icd_downcast(cl_device_id device) {
    return static_cast<cvk_device*>(device);
}

struct cvk_platform : public _cl_platform_id {
    cvk_platform() {
        m_extensions = {
            MAKE_NAME_VERSION(1, 0, 0, "cl_khr_icd"),
            MAKE_NAME_VERSION(1, 0, 0, "cl_khr_extended_versioning"),
        };

        for (auto& ext : m_extensions) {
            m_extension_string += ext.name;
            m_extension_string += " ";
        }
    }
    ~cvk_platform() {
        for (auto dev : m_devices) {
            delete dev;
        }
    }

    CHECK_RETURN bool create_device(VkPhysicalDevice pdev) {
        auto dev = cvk_device::create(pdev);
        if (dev != nullptr) {
            m_devices.push_back(dev);
            return true;
        } else {
            return false;
        }
    }

    cl_version_khr version() const { return gOpenCLVersion; }

    std::string version_string() const {
        std::string ret = "OpenCL ";
        auto ver = version();
        ret += std::to_string(CL_VERSION_MAJOR_KHR(ver));
        ret += ".";
        ret += std::to_string(CL_VERSION_MINOR_KHR(ver));
        ret += " clvk";
        return ret;
    }

    std::string name() const { return "clvk"; }

    std::string vendor() const { return "clvk"; }

    std::string profile() const { return "FULL_PROFILE"; }

    std::string icd_suffix() const { return "clvk"; }

    const std::vector<cvk_device*>& devices() const { return m_devices; }

    const std::vector<cl_name_version_khr>& extensions() const {
        return m_extensions;
    }

    const std::string& extension_string() const { return m_extension_string; }

private:
    std::vector<cl_name_version_khr> m_extensions;
    std::string m_extension_string;
    std::vector<cvk_device*> m_devices;
};

static inline cvk_platform* icd_downcast(cl_platform_id platform) {
    return static_cast<cvk_platform*>(platform);
}
