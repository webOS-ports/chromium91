// Copyright (c) 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/vulkan/vulkan_instance.h"

#include <vector>

#include "base/containers/flat_set.h"
#include "base/logging.h"
#include "base/macros.h"
#include "build/build_config.h"
#include "gpu/vulkan/vulkan_crash_keys.h"
#include "gpu/vulkan/vulkan_device_queue.h"
#include "gpu/vulkan/vulkan_function_pointers.h"
#include "gpu/vulkan/vulkan_util.h"

namespace gpu {

namespace {

#if DCHECK_IS_ON()
const char* kSkippedErrors[] = {
    // http://anglebug.com/4583
    "VUID-VkGraphicsPipelineCreateInfo-blendEnable-02023",
};

VKAPI_ATTR VkBool32 VKAPI_CALL
VulkanErrorCallback(VkDebugReportFlagsEXT flags,
                    VkDebugReportObjectTypeEXT object_type,
                    uint64_t object,
                    size_t location,
                    int32_t message_code,
                    const char* layer_prefix,
                    const char* message,
                    void* user_data) {
  static base::flat_set<const char*> hitted_errors;
  for (const char* error : kSkippedErrors) {
    if (strstr(message, error) != nullptr) {
      if (hitted_errors.find(error) != hitted_errors.end())
        return VK_FALSE;
      hitted_errors.insert(error);
    }
  }
  LOG(ERROR) << message;
  return VK_FALSE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL
VulkanWarningCallback(VkDebugReportFlagsEXT flags,
                      VkDebugReportObjectTypeEXT object_type,
                      uint64_t object,
                      size_t location,
                      int32_t message_code,
                      const char* layer_prefix,
                      const char* message,
                      void* user_data) {
  LOG(WARNING) << message;
  return VK_FALSE;
}
#endif  // DCHECK_IS_ON()

}  // namespace

VulkanInstance::VulkanInstance() = default;

VulkanInstance::~VulkanInstance() {
  Destroy();
}

bool VulkanInstance::Initialize(
    const std::vector<const char*>& required_extensions,
    const std::vector<const char*>& required_layers) {
  DCHECK(!vk_instance_);

  VulkanFunctionPointers* vulkan_function_pointers =
      gpu::GetVulkanFunctionPointers();

  if (!vulkan_function_pointers->BindUnassociatedFunctionPointers())
    return false;

  VkResult result = vkEnumerateInstanceVersion(&vulkan_info_.api_version);
  if (result != VK_SUCCESS) {
    DLOG(ERROR) << "vkEnumerateInstanceVersion() failed: " << result;
    return false;
  }

  if (vulkan_info_.api_version < kVulkanRequiredApiVersion)
    return false;

  gpu::crash_keys::vulkan_api_version.Set(
      VkVersionToString(vulkan_info_.api_version));

  vulkan_info_.used_api_version = kVulkanRequiredApiVersion;

  VkApplicationInfo app_info = {};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "Chromium";
  app_info.apiVersion = vulkan_info_.used_api_version;

  // Query the extensions from all layers, including ones that are implicitly
  // available (identified by passing a null ptr as the layer name).
  std::vector<const char*> all_required_layers = required_layers;

  // Include the extension properties provided by the Vulkan implementation as
  // part of the enumeration.
  all_required_layers.push_back(nullptr);

  for (const char* layer_name : all_required_layers) {
    vulkan_info_.enabled_instance_extensions = required_extensions;
    uint32_t num_instance_exts = 0;
    result = vkEnumerateInstanceExtensionProperties(
        layer_name, &num_instance_exts, nullptr);
    if (VK_SUCCESS != result) {
      DLOG(ERROR) << "vkEnumerateInstanceExtensionProperties(" << layer_name
                  << ") failed: " << result;
      return false;
    }

    const size_t previous_extension_count =
        vulkan_info_.instance_extensions.size();
    vulkan_info_.instance_extensions.resize(previous_extension_count +
                                            num_instance_exts);
    result = vkEnumerateInstanceExtensionProperties(
        layer_name, &num_instance_exts,
        &vulkan_info_.instance_extensions.data()[previous_extension_count]);
    if (VK_SUCCESS != result) {
      DLOG(ERROR) << "vkEnumerateInstanceExtensionProperties(" << layer_name
                  << ") failed: " << result;
      return false;
    }
  }

  for (const VkExtensionProperties& ext_property :
       vulkan_info_.instance_extensions) {
    if (strcmp(ext_property.extensionName,
               VK_EXT_DEBUG_REPORT_EXTENSION_NAME) == 0) {
      debug_report_enabled_ = true;
      vulkan_info_.enabled_instance_extensions.push_back(
          VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
    }
  }

#if DCHECK_IS_ON()
  for (const char* enabled_extension :
       vulkan_info_.enabled_instance_extensions) {
    bool found = false;
    for (const VkExtensionProperties& ext_property :
         vulkan_info_.instance_extensions) {
      if (strcmp(ext_property.extensionName, enabled_extension) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      DLOG(ERROR) << "Required extension " << enabled_extension
                  << " missing from enumerated Vulkan extensions. "
                     "vkCreateInstance will likely fail.";
    }
  }
#endif

  std::vector<const char*> enabled_layer_names = required_layers;
  uint32_t num_instance_layers = 0;
  result = vkEnumerateInstanceLayerProperties(&num_instance_layers, nullptr);
  if (VK_SUCCESS != result) {
    DLOG(ERROR) << "vkEnumerateInstanceLayerProperties(NULL) failed: "
                << result;
    return false;
  }

  vulkan_info_.instance_layers.resize(num_instance_layers);
  result = vkEnumerateInstanceLayerProperties(
      &num_instance_layers, vulkan_info_.instance_layers.data());
  if (VK_SUCCESS != result) {
    DLOG(ERROR) << "vkEnumerateInstanceLayerProperties() failed: " << result;
    return false;
  }

  VkInstanceCreateInfo instance_create_info = {
      VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,           // sType
      nullptr,                                          // pNext
      0,                                                // flags
      &app_info,                                        // pApplicationInfo
      enabled_layer_names.size(),                       // enableLayerCount
      enabled_layer_names.data(),                       // ppEnabledLayerNames
      vulkan_info_.enabled_instance_extensions.size(),  // enabledExtensionCount
      vulkan_info_.enabled_instance_extensions
          .data(),  // ppEnabledExtensionNames
  };

  result = vkCreateInstance(&instance_create_info, nullptr, &vk_instance_);
  if (VK_SUCCESS != result) {
    DLOG(ERROR) << "vkCreateInstance() failed: " << result;
    return false;
  }

  gfx::ExtensionSet enabled_extensions(
      std::begin(vulkan_info_.enabled_instance_extensions),
      std::end(vulkan_info_.enabled_instance_extensions));

  if (!vulkan_function_pointers->BindInstanceFunctionPointers(
          vk_instance_, vulkan_info_.used_api_version, enabled_extensions)) {
    return false;
  }

#if DCHECK_IS_ON()
  // Register our error logging function.
  if (debug_report_enabled_) {
    VkDebugReportCallbackCreateInfoEXT cb_create_info = {};
    cb_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CREATE_INFO_EXT;

    cb_create_info.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT;
    cb_create_info.pfnCallback = &VulkanErrorCallback;
    result = vkCreateDebugReportCallbackEXT(vk_instance_, &cb_create_info,
                                            nullptr, &error_callback_);
    if (VK_SUCCESS != result) {
      error_callback_ = VK_NULL_HANDLE;
      DLOG(ERROR) << "vkCreateDebugReportCallbackEXT(ERROR) failed: " << result;
      return false;
    }

    cb_create_info.flags = VK_DEBUG_REPORT_WARNING_BIT_EXT |
                           VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
    cb_create_info.pfnCallback = &VulkanWarningCallback;
    result = vkCreateDebugReportCallbackEXT(vk_instance_, &cb_create_info,
                                            nullptr, &warning_callback_);
    if (VK_SUCCESS != result) {
      warning_callback_ = VK_NULL_HANDLE;
      DLOG(ERROR) << "vkCreateDebugReportCallbackEXT(WARN) failed: " << result;
      return false;
    }
  }
#endif

  if (!CollectInfo())
    return false;
  return true;
}

bool VulkanInstance::CollectInfo() {
  uint32_t count = 0;
  VkResult result = vkEnumeratePhysicalDevices(vk_instance_, &count, nullptr);
  if (result != VK_SUCCESS) {
    DLOG(ERROR) << "vkEnumeratePhysicalDevices failed: " << result;
    return false;
  }

  if (!count) {
    DLOG(ERROR) << "vkEnumeratePhysicalDevices returns zero device.";
    return false;
  }

  std::vector<VkPhysicalDevice> physical_devices(count);
  result =
      vkEnumeratePhysicalDevices(vk_instance_, &count, physical_devices.data());
  if (VK_SUCCESS != result) {
    DLOG(ERROR) << "vkEnumeratePhysicalDevices() failed: " << result;
    return false;
  }

  vulkan_info_.physical_devices.reserve(count);
  for (VkPhysicalDevice device : physical_devices) {
    vulkan_info_.physical_devices.emplace_back();
    auto& info = vulkan_info_.physical_devices.back();
    info.device = device;

    vkGetPhysicalDeviceProperties(device, &info.properties);

    count = 0;
    result = vkEnumerateDeviceExtensionProperties(
        device, nullptr /* pLayerName */, &count, nullptr);
    DLOG_IF(ERROR, result != VK_SUCCESS)
        << "vkEnumerateDeviceExtensionProperties failed: " << result;

    info.extensions.resize(count);
    result = vkEnumerateDeviceExtensionProperties(
        device, nullptr /* pLayerName */, &count, info.extensions.data());
    DLOG_IF(ERROR, result != VK_SUCCESS)
        << "vkEnumerateDeviceExtensionProperties failed: " << result;

    // The API version of the VkInstance might be different than the supported
    // API version of the VkPhysicalDevice, so we need to check the GPU's
    // API version instead of just testing to see if
    // vkGetPhysicalDeviceProperties2 and vkGetPhysicalDeviceFeatures2 are
    // non-null.
    static_assert(kVulkanRequiredApiVersion >= VK_API_VERSION_1_1, "");
    if (info.properties.apiVersion >= kVulkanRequiredApiVersion) {
      info.driver_properties = VkPhysicalDeviceDriverProperties{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
      };

      VkPhysicalDeviceProperties2 properties2 = {
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
          .pNext = &info.driver_properties,
      };
      vkGetPhysicalDeviceProperties2(device, &properties2);

      VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_conversion_features =
          {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
      VkPhysicalDeviceProtectedMemoryFeatures protected_memory_feature = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES};
      VkPhysicalDeviceFeatures2 features_2 = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      features_2.pNext = &ycbcr_conversion_features;
      ycbcr_conversion_features.pNext = &protected_memory_feature;

      vkGetPhysicalDeviceFeatures2(device, &features_2);
      info.features = features_2.features;
      info.feature_sampler_ycbcr_conversion =
          ycbcr_conversion_features.samplerYcbcrConversion;
      info.feature_protected_memory = protected_memory_feature.protectedMemory;
    }

    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    if (count) {
      info.queue_families.resize(count);
      vkGetPhysicalDeviceQueueFamilyProperties(device, &count,
                                               info.queue_families.data());
    }
  }
  return true;
}

void VulkanInstance::Destroy() {
#if DCHECK_IS_ON()
  if (debug_report_enabled_ && (error_callback_ != VK_NULL_HANDLE ||
                                warning_callback_ != VK_NULL_HANDLE)) {
    if (error_callback_ != VK_NULL_HANDLE) {
      vkDestroyDebugReportCallbackEXT(vk_instance_, error_callback_, nullptr);
      error_callback_ = VK_NULL_HANDLE;
    }
    if (warning_callback_ != VK_NULL_HANDLE) {
      vkDestroyDebugReportCallbackEXT(vk_instance_, warning_callback_, nullptr);
      warning_callback_ = VK_NULL_HANDLE;
    }
  }
#endif
  if (vk_instance_ != VK_NULL_HANDLE) {
    vkDestroyInstance(vk_instance_, nullptr);
    vk_instance_ = VK_NULL_HANDLE;
  }
  VulkanFunctionPointers* vulkan_function_pointers =
      gpu::GetVulkanFunctionPointers();
  if (vulkan_function_pointers->vulkan_loader_library) {
    base::UnloadNativeLibrary(vulkan_function_pointers->vulkan_loader_library);
    vulkan_function_pointers->vulkan_loader_library = nullptr;
  }
}

}  // namespace gpu
