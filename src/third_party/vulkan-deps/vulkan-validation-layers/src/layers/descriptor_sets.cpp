/* Copyright (c) 2015-2021 The Khronos Group Inc.
 * Copyright (c) 2015-2021 Valve Corporation
 * Copyright (c) 2015-2021 LunarG, Inc.
 * Copyright (C) 2015-2021 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Tobin Ehlis <tobine@google.com>
 *         John Zulauf <jzulauf@lunarg.com>
 *         Jeremy Kniager <jeremyk@lunarg.com>
 */

#include "chassis.h"
#include "core_validation_error_enums.h"
#include "core_validation.h"
#include "descriptor_sets.h"
#include "hash_vk_types.h"
#include "vk_enum_string_helper.h"
#include "vk_safe_struct.h"
#include "vk_typemap_helper.h"
#include "buffer_validation.h"
#include <sstream>
#include <algorithm>
#include <array>
#include <memory>

// ExtendedBinding collects a VkDescriptorSetLayoutBinding and any extended
// state that comes from a different array/structure so they can stay together
// while being sorted by binding number.
struct ExtendedBinding {
    ExtendedBinding(const VkDescriptorSetLayoutBinding *l, VkDescriptorBindingFlags f) : layout_binding(l), binding_flags(f) {}

    const VkDescriptorSetLayoutBinding *layout_binding;
    VkDescriptorBindingFlags binding_flags;
};

struct BindingNumCmp {
    bool operator()(const ExtendedBinding &a, const ExtendedBinding &b) const {
        return a.layout_binding->binding < b.layout_binding->binding;
    }
};

using DescriptorSet = cvdescriptorset::DescriptorSet;
using DescriptorSetLayout = cvdescriptorset::DescriptorSetLayout;
using DescriptorSetLayoutDef = cvdescriptorset::DescriptorSetLayoutDef;
using DescriptorSetLayoutId = cvdescriptorset::DescriptorSetLayoutId;

// Canonical dictionary of DescriptorSetLayoutDef (without any handle/device specific information)
cvdescriptorset::DescriptorSetLayoutDict descriptor_set_layout_dict;

DescriptorSetLayoutId GetCanonicalId(const VkDescriptorSetLayoutCreateInfo *p_create_info) {
    return descriptor_set_layout_dict.look_up(DescriptorSetLayoutDef(p_create_info));
}

// Construct DescriptorSetLayout instance from given create info
// Proactively reserve and resize as possible, as the reallocation was visible in profiling
cvdescriptorset::DescriptorSetLayoutDef::DescriptorSetLayoutDef(const VkDescriptorSetLayoutCreateInfo *p_create_info)
    : flags_(p_create_info->flags), binding_count_(0), descriptor_count_(0), dynamic_descriptor_count_(0) {
    const auto *flags_create_info = LvlFindInChain<VkDescriptorSetLayoutBindingFlagsCreateInfo>(p_create_info->pNext);

    binding_type_stats_ = {0, 0};
    std::set<ExtendedBinding, BindingNumCmp> sorted_bindings;
    const uint32_t input_bindings_count = p_create_info->bindingCount;
    // Sort the input bindings in binding number order, eliminating duplicates
    for (uint32_t i = 0; i < input_bindings_count; i++) {
        VkDescriptorBindingFlags flags = 0;
        if (flags_create_info && flags_create_info->bindingCount == p_create_info->bindingCount) {
            flags = flags_create_info->pBindingFlags[i];
        }
        sorted_bindings.emplace(p_create_info->pBindings + i, flags);
    }

    // Store the create info in the sorted order from above
    uint32_t index = 0;
    binding_count_ = static_cast<uint32_t>(sorted_bindings.size());
    bindings_.reserve(binding_count_);
    binding_flags_.reserve(binding_count_);
    binding_to_index_map_.reserve(binding_count_);
    for (const auto &input_binding : sorted_bindings) {
        // Add to binding and map, s.t. it is robust to invalid duplication of binding_num
        const auto binding_num = input_binding.layout_binding->binding;
        binding_to_index_map_[binding_num] = index++;
        bindings_.emplace_back(input_binding.layout_binding);
        auto &binding_info = bindings_.back();
        binding_flags_.emplace_back(input_binding.binding_flags);

        descriptor_count_ += binding_info.descriptorCount;
        if (binding_info.descriptorCount > 0) {
            non_empty_bindings_.insert(binding_num);
        }

        if (IsDynamicDescriptor(binding_info.descriptorType)) {
            dynamic_descriptor_count_ += binding_info.descriptorCount;
        }

        // Get stats depending on descriptor type for caching later
        if (IsBufferDescriptor(binding_info.descriptorType)) {
            if (IsDynamicDescriptor(binding_info.descriptorType)) {
                binding_type_stats_.dynamic_buffer_count++;
            } else {
                binding_type_stats_.non_dynamic_buffer_count++;
            }
        }
    }
    assert(bindings_.size() == binding_count_);
    assert(binding_flags_.size() == binding_count_);
    uint32_t global_index = 0;
    global_index_range_.reserve(binding_count_);
    // Vector order is finalized so build vectors of descriptors and dynamic offsets by binding index
    for (uint32_t i = 0; i < binding_count_; ++i) {
        auto final_index = global_index + bindings_[i].descriptorCount;
        global_index_range_.emplace_back(global_index, final_index);
        global_index = final_index;
    }
}

size_t cvdescriptorset::DescriptorSetLayoutDef::hash() const {
    hash_util::HashCombiner hc;
    hc << flags_;
    hc.Combine(bindings_);
    hc.Combine(binding_flags_);
    return hc.Value();
}
//

// Return valid index or "end" i.e. binding_count_;
// The asserts in "Get" are reduced to the set where no valid answer(like null or 0) could be given
// Common code for all binding lookups.
uint32_t cvdescriptorset::DescriptorSetLayoutDef::GetIndexFromBinding(uint32_t binding) const {
    const auto &bi_itr = binding_to_index_map_.find(binding);
    if (bi_itr != binding_to_index_map_.cend()) return bi_itr->second;
    return GetBindingCount();
}
VkDescriptorSetLayoutBinding const *cvdescriptorset::DescriptorSetLayoutDef::GetDescriptorSetLayoutBindingPtrFromIndex(
    const uint32_t index) const {
    if (index >= bindings_.size()) return nullptr;
    return bindings_[index].ptr();
}
// Return descriptorCount for given index, 0 if index is unavailable
uint32_t cvdescriptorset::DescriptorSetLayoutDef::GetDescriptorCountFromIndex(const uint32_t index) const {
    if (index >= bindings_.size()) return 0;
    return bindings_[index].descriptorCount;
}
// For the given index, return descriptorType
VkDescriptorType cvdescriptorset::DescriptorSetLayoutDef::GetTypeFromIndex(const uint32_t index) const {
    assert(index < bindings_.size());
    if (index < bindings_.size()) return bindings_[index].descriptorType;
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}
// For the given index, return stageFlags
VkShaderStageFlags cvdescriptorset::DescriptorSetLayoutDef::GetStageFlagsFromIndex(const uint32_t index) const {
    assert(index < bindings_.size());
    if (index < bindings_.size()) return bindings_[index].stageFlags;
    return VkShaderStageFlags(0);
}
// Return binding flags for given index, 0 if index is unavailable
VkDescriptorBindingFlags cvdescriptorset::DescriptorSetLayoutDef::GetDescriptorBindingFlagsFromIndex(const uint32_t index) const {
    if (index >= binding_flags_.size()) return 0;
    return binding_flags_[index];
}

const cvdescriptorset::IndexRange &cvdescriptorset::DescriptorSetLayoutDef::GetGlobalIndexRangeFromIndex(uint32_t index) const {
    const static IndexRange k_invalid_range = {0xFFFFFFFF, 0xFFFFFFFF};
    if (index >= binding_flags_.size()) return k_invalid_range;
    return global_index_range_[index];
}

// For the given binding, return the global index range (half open)
// As start and end are often needed in pairs, get both with a single lookup.
const cvdescriptorset::IndexRange &cvdescriptorset::DescriptorSetLayoutDef::GetGlobalIndexRangeFromBinding(
    const uint32_t binding) const {
    uint32_t index = GetIndexFromBinding(binding);
    return GetGlobalIndexRangeFromIndex(index);
}

// For given binding, return ptr to ImmutableSampler array
VkSampler const *cvdescriptorset::DescriptorSetLayoutDef::GetImmutableSamplerPtrFromBinding(const uint32_t binding) const {
    const auto &bi_itr = binding_to_index_map_.find(binding);
    if (bi_itr != binding_to_index_map_.end()) {
        return bindings_[bi_itr->second].pImmutableSamplers;
    }
    return nullptr;
}
// Move to next valid binding having a non-zero binding count
uint32_t cvdescriptorset::DescriptorSetLayoutDef::GetNextValidBinding(const uint32_t binding) const {
    auto it = non_empty_bindings_.upper_bound(binding);
    assert(it != non_empty_bindings_.cend());
    if (it != non_empty_bindings_.cend()) return *it;
    return GetMaxBinding() + 1;
}
// For given index, return ptr to ImmutableSampler array
VkSampler const *cvdescriptorset::DescriptorSetLayoutDef::GetImmutableSamplerPtrFromIndex(const uint32_t index) const {
    if (index < bindings_.size()) {
        return bindings_[index].pImmutableSamplers;
    }
    return nullptr;
}

// If our layout is compatible with rh_ds_layout, return true.
bool cvdescriptorset::DescriptorSetLayout::IsCompatible(DescriptorSetLayout const *rh_ds_layout) const {
    bool compatible = (this == rh_ds_layout) || (GetLayoutDef() == rh_ds_layout->GetLayoutDef());
    return compatible;
}

// TODO: Find a way to add smarts to the autogenerated version of this
static std::string smart_string_VkShaderStageFlags(VkShaderStageFlags stage_flags) {
    if (stage_flags == VK_SHADER_STAGE_ALL) {
        return string_VkShaderStageFlagBits(VK_SHADER_STAGE_ALL);
    }

    return string_VkShaderStageFlags(stage_flags);
}

// If our layout is compatible with bound_dsl, return true,
//  else return false and fill in error_msg will description of what causes incompatibility
bool cvdescriptorset::VerifySetLayoutCompatibility(const debug_report_data *report_data, DescriptorSetLayout const *layout_dsl,
                                                   DescriptorSetLayout const *bound_dsl, std::string *error_msg) {
    // Short circuit the detailed check.
    if (layout_dsl->IsCompatible(bound_dsl)) return true;

    // Do a detailed compatibility check of this lhs def (referenced by layout_dsl), vs. the rhs (layout and def)
    // Should only be run if trivial accept has failed, and in that context should return false.
    VkDescriptorSetLayout layout_dsl_handle = layout_dsl->GetDescriptorSetLayout();
    VkDescriptorSetLayout bound_dsl_handle = bound_dsl->GetDescriptorSetLayout();
    DescriptorSetLayoutDef const *layout_ds_layout_def = layout_dsl->GetLayoutDef();
    DescriptorSetLayoutDef const *bound_ds_layout_def = bound_dsl->GetLayoutDef();

    // Check descriptor counts
    const auto bound_total_count = bound_ds_layout_def->GetTotalDescriptorCount();
    if (layout_ds_layout_def->GetTotalDescriptorCount() != bound_ds_layout_def->GetTotalDescriptorCount()) {
        std::stringstream error_str;
        error_str << report_data->FormatHandle(layout_dsl_handle) << " from pipeline layout has "
                  << layout_ds_layout_def->GetTotalDescriptorCount() << " total descriptors, but "
                  << report_data->FormatHandle(bound_dsl_handle) << ", which is bound, has " << bound_total_count
                  << " total descriptors.";
        *error_msg = error_str.str();
        return false;  // trivial fail case
    }

    // Descriptor counts match so need to go through bindings one-by-one
    //  and verify that type and stageFlags match
    for (const auto &layout_binding : layout_ds_layout_def->GetBindings()) {
        // TODO : Do we also need to check immutable samplers?
        const auto bound_binding = bound_ds_layout_def->GetBindingInfoFromBinding(layout_binding.binding);
        if (layout_binding.descriptorCount != bound_binding->descriptorCount) {
            std::stringstream error_str;
            error_str << "Binding " << layout_binding.binding << " for " << report_data->FormatHandle(layout_dsl_handle)
                      << " from pipeline layout has a descriptorCount of " << layout_binding.descriptorCount << " but binding "
                      << layout_binding.binding << " for " << report_data->FormatHandle(bound_dsl_handle)
                      << ", which is bound, has a descriptorCount of " << bound_binding->descriptorCount;
            *error_msg = error_str.str();
            return false;
        } else if (layout_binding.descriptorType != bound_binding->descriptorType) {
            std::stringstream error_str;
            error_str << "Binding " << layout_binding.binding << " for " << report_data->FormatHandle(layout_dsl_handle)
                      << " from pipeline layout is type '" << string_VkDescriptorType(layout_binding.descriptorType)
                      << "' but binding " << layout_binding.binding << " for " << report_data->FormatHandle(bound_dsl_handle)
                      << ", which is bound, is type '" << string_VkDescriptorType(bound_binding->descriptorType) << "'";
            *error_msg = error_str.str();
            return false;
        } else if (layout_binding.stageFlags != bound_binding->stageFlags) {
            std::stringstream error_str;
            error_str << "Binding " << layout_binding.binding << " for " << report_data->FormatHandle(layout_dsl_handle)
                      << " from pipeline layout has stageFlags " << smart_string_VkShaderStageFlags(layout_binding.stageFlags)
                      << " but binding " << layout_binding.binding << " for " << report_data->FormatHandle(bound_dsl_handle)
                      << ", which is bound, has stageFlags " << smart_string_VkShaderStageFlags(bound_binding->stageFlags);
            *error_msg = error_str.str();
            return false;
        }
    }

    const auto &ds_layout_flags = layout_ds_layout_def->GetBindingFlags();
    const auto &bound_layout_flags = bound_ds_layout_def->GetBindingFlags();
    if (bound_layout_flags != ds_layout_flags) {
        std::stringstream error_str;
        assert(ds_layout_flags.size() == bound_layout_flags.size());
        size_t i;
        for (i = 0; i < ds_layout_flags.size(); i++) {
            if (ds_layout_flags[i] != bound_layout_flags[i]) break;
        }
        error_str << report_data->FormatHandle(layout_dsl_handle)
                  << " from pipeline layout does not have the same binding flags at binding " << i << " ( "
                  << string_VkDescriptorBindingFlagsEXT(ds_layout_flags[i]) << " ) as "
                  << report_data->FormatHandle(bound_dsl_handle) << " ( "
                  << string_VkDescriptorBindingFlagsEXT(bound_layout_flags[i]) << " ), which is bound";
        *error_msg = error_str.str();
        return false;
    }

    // No detailed check should succeed if the trivial check failed -- or the dictionary has failed somehow.
    bool compatible = true;
    assert(!compatible);
    return compatible;
}

bool cvdescriptorset::DescriptorSetLayoutDef::IsNextBindingConsistent(const uint32_t binding) const {
    if (!binding_to_index_map_.count(binding + 1)) return false;
    auto const &bi_itr = binding_to_index_map_.find(binding);
    if (bi_itr != binding_to_index_map_.end()) {
        const auto &next_bi_itr = binding_to_index_map_.find(binding + 1);
        if (next_bi_itr != binding_to_index_map_.end()) {
            auto type = bindings_[bi_itr->second].descriptorType;
            auto stage_flags = bindings_[bi_itr->second].stageFlags;
            auto immut_samp = bindings_[bi_itr->second].pImmutableSamplers ? true : false;
            auto flags = binding_flags_[bi_itr->second];
            if ((type != bindings_[next_bi_itr->second].descriptorType) ||
                (stage_flags != bindings_[next_bi_itr->second].stageFlags) ||
                (immut_samp != (bindings_[next_bi_itr->second].pImmutableSamplers ? true : false)) ||
                (flags != binding_flags_[next_bi_itr->second])) {
                return false;
            }
            return true;
        }
    }
    return false;
}

// The DescriptorSetLayout stores the per handle data for a descriptor set layout, and references the common defintion for the
// handle invariant portion
cvdescriptorset::DescriptorSetLayout::DescriptorSetLayout(const VkDescriptorSetLayoutCreateInfo *p_create_info,
                                                          const VkDescriptorSetLayout layout)
    : layout_(layout), layout_id_(GetCanonicalId(p_create_info)) {}

// Validate descriptor set layout create info
bool cvdescriptorset::ValidateDescriptorSetLayoutCreateInfo(
    const ValidationObject *val_obj, const VkDescriptorSetLayoutCreateInfo *create_info, const bool push_descriptor_ext,
    const uint32_t max_push_descriptors, const bool descriptor_indexing_ext,
    const VkPhysicalDeviceVulkan12Features *core12_features,
    const VkPhysicalDeviceInlineUniformBlockFeaturesEXT *inline_uniform_block_features,
    const VkPhysicalDeviceInlineUniformBlockPropertiesEXT *inline_uniform_block_props, const DeviceExtensions *device_extensions) {
    bool skip = false;
    layer_data::unordered_set<uint32_t> bindings;
    uint64_t total_descriptors = 0;

    const auto *flags_create_info = LvlFindInChain<VkDescriptorSetLayoutBindingFlagsCreateInfo>(create_info->pNext);

    const bool push_descriptor_set = !!(create_info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
    if (push_descriptor_set && !push_descriptor_ext) {
        skip |= val_obj->LogError(
            val_obj->device, kVUID_Core_DrawState_ExtensionNotEnabled,
            "vkCreateDescriptorSetLayout(): Attempted to use %s in %s but its required extension %s has not been enabled.\n",
            "VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR", "VkDescriptorSetLayoutCreateInfo::flags",
            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    }

    const bool update_after_bind_set = !!(create_info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);
    if (update_after_bind_set && !descriptor_indexing_ext) {
        skip |= val_obj->LogError(
            val_obj->device, kVUID_Core_DrawState_ExtensionNotEnabled,
            "vkCreateDescriptorSetLayout(): Attemped to use %s in %s but its required extension %s has not been enabled.\n",
            "VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT", "VkDescriptorSetLayoutCreateInfo::flags",
            VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    }

    auto valid_type = [push_descriptor_set](const VkDescriptorType type) {
        return !push_descriptor_set ||
               ((type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) && (type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) &&
                (type != VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT));
    };

    uint32_t max_binding = 0;

    for (uint32_t i = 0; i < create_info->bindingCount; ++i) {
        const auto &binding_info = create_info->pBindings[i];
        max_binding = std::max(max_binding, binding_info.binding);

        if (!bindings.insert(binding_info.binding).second) {
            skip |= val_obj->LogError(val_obj->device, "VUID-VkDescriptorSetLayoutCreateInfo-binding-00279",
                                      "vkCreateDescriptorSetLayout(): pBindings[%u] has duplicated binding number (%u).", i,
                                      binding_info.binding);
        }
        if (!valid_type(binding_info.descriptorType)) {
            skip |= val_obj->LogError(val_obj->device,
                                      (binding_info.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT)
                                          ? "VUID-VkDescriptorSetLayoutCreateInfo-flags-02208"
                                          : "VUID-VkDescriptorSetLayoutCreateInfo-flags-00280",
                                      "vkCreateDescriptorSetLayout(): pBindings[%u] has invalid type %s , for push descriptors.", i,
                                      string_VkDescriptorType(binding_info.descriptorType));
        }

        if (binding_info.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT) {
            if (!inline_uniform_block_features->inlineUniformBlock) {
                skip |= val_obj->LogError(val_obj->device, "VUID-VkDescriptorSetLayoutBinding-descriptorType-04604",
                                          "vkCreateDescriptorSetLayout(): pBindings[%u] is creating VkDescriptorSetLayout with "
                                          "descriptor type VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT "
                                          "but inlineUniformBlock is not enabled",
                                          i, VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME);
            } else {
                if ((binding_info.descriptorCount % 4) != 0) {
                    skip |= val_obj->LogError(val_obj->device, "VUID-VkDescriptorSetLayoutBinding-descriptorType-02209",
                                              "vkCreateDescriptorSetLayout(): pBindings[%u] has descriptorCount =(%" PRIu32
                                              ") but must be a multiple of 4",
                                              i, binding_info.descriptorCount);
                }
                if (binding_info.descriptorCount > inline_uniform_block_props->maxInlineUniformBlockSize) {
                    skip |=
                        val_obj->LogError(val_obj->device, "VUID-VkDescriptorSetLayoutBinding-descriptorType-02210",
                                          "vkCreateDescriptorSetLayout(): pBindings[%u] has descriptorCount =(%" PRIu32
                                          ") but must be less than or equal to maxInlineUniformBlockSize (%u)",
                                          i, binding_info.descriptorCount, inline_uniform_block_props->maxInlineUniformBlockSize);
                }
            }
        }

        if ((binding_info.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
             binding_info.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
            binding_info.pImmutableSamplers && device_extensions->vk_ext_custom_border_color) {
            const CoreChecks *core_checks = reinterpret_cast<const CoreChecks *>(val_obj);
            for (uint32_t j = 0; j < binding_info.descriptorCount; j++) {
                const SAMPLER_STATE *sampler_state = core_checks->GetSamplerState(binding_info.pImmutableSamplers[j]);
                if (sampler_state && (sampler_state->createInfo.borderColor == VK_BORDER_COLOR_INT_CUSTOM_EXT ||
                                      sampler_state->createInfo.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT)) {
                    skip |= val_obj->LogError(
                        val_obj->device, "VUID-VkDescriptorSetLayoutBinding-pImmutableSamplers-04009",
                        "vkCreateDescriptorSetLayout(): pBindings[%u].pImmutableSamplers[%u] has VkSampler %" PRIu64
                        " presented as immutable has a custom border color",
                        i, j, binding_info.pImmutableSamplers[j]);
                }
            }
        }

        total_descriptors += binding_info.descriptorCount;
    }

    if (flags_create_info) {
        if (flags_create_info->bindingCount != 0 && flags_create_info->bindingCount != create_info->bindingCount) {
            skip |= val_obj->LogError(val_obj->device, "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-bindingCount-03002",
                                      "vkCreateDescriptorSetLayout(): VkDescriptorSetLayoutCreateInfo::bindingCount (%d) != "
                                      "VkDescriptorSetLayoutBindingFlagsCreateInfo::bindingCount (%d)",
                                      create_info->bindingCount, flags_create_info->bindingCount);
        }

        if (flags_create_info->bindingCount == create_info->bindingCount) {
            for (uint32_t i = 0; i < create_info->bindingCount; ++i) {
                const auto &binding_info = create_info->pBindings[i];

                if (flags_create_info->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) {
                    if (!update_after_bind_set) {
                        skip |= val_obj->LogError(val_obj->device, "VUID-VkDescriptorSetLayoutCreateInfo-flags-03000",
                                                  "vkCreateDescriptorSetLayout(): pBindings[%u] does not have "
                                                  "VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT.",
                                                  i);
                    }

                    if (binding_info.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                        !core12_features->descriptorBindingUniformBufferUpdateAfterBind) {
                        skip |= val_obj->LogError(
                            val_obj->device,
                            "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-"
                            "descriptorBindingUniformBufferUpdateAfterBind-03005",
                            "vkCreateDescriptorSetLayout(): pBindings[%u] can't have VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT "
                            "for %s since descriptorBindingUniformBufferUpdateAfterBind is not enabled.",
                            i, string_VkDescriptorType(binding_info.descriptorType));
                    }
                    if ((binding_info.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                         binding_info.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                         binding_info.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) &&
                        !core12_features->descriptorBindingSampledImageUpdateAfterBind) {
                        skip |= val_obj->LogError(
                            val_obj->device,
                            "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-"
                            "descriptorBindingSampledImageUpdateAfterBind-03006",
                            "vkCreateDescriptorSetLayout(): pBindings[%u] can't have VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT "
                            "for %s since descriptorBindingSampledImageUpdateAfterBind is not enabled.",
                            i, string_VkDescriptorType(binding_info.descriptorType));
                    }
                    if (binding_info.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
                        !core12_features->descriptorBindingStorageImageUpdateAfterBind) {
                        skip |= val_obj->LogError(
                            val_obj->device,
                            "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-"
                            "descriptorBindingStorageImageUpdateAfterBind-03007",
                            "vkCreateDescriptorSetLayout(): pBindings[%u] can't have VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT "
                            "for %s since descriptorBindingStorageImageUpdateAfterBind is not enabled.",
                            i, string_VkDescriptorType(binding_info.descriptorType));
                    }
                    if (binding_info.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                        !core12_features->descriptorBindingStorageBufferUpdateAfterBind) {
                        skip |= val_obj->LogError(
                            val_obj->device,
                            "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-"
                            "descriptorBindingStorageBufferUpdateAfterBind-03008",
                            "vkCreateDescriptorSetLayout(): pBindings[%u] can't have VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT "
                            "for %s since descriptorBindingStorageBufferUpdateAfterBind is not enabled.",
                            i, string_VkDescriptorType(binding_info.descriptorType));
                    }
                    if (binding_info.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
                        !core12_features->descriptorBindingUniformTexelBufferUpdateAfterBind) {
                        skip |= val_obj->LogError(
                            val_obj->device,
                            "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-"
                            "descriptorBindingUniformTexelBufferUpdateAfterBind-03009",
                            "vkCreateDescriptorSetLayout(): pBindings[%u] can't have VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT "
                            "for %s since descriptorBindingUniformTexelBufferUpdateAfterBind is not enabled.",
                            i, string_VkDescriptorType(binding_info.descriptorType));
                    }
                    if (binding_info.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER &&
                        !core12_features->descriptorBindingStorageTexelBufferUpdateAfterBind) {
                        skip |= val_obj->LogError(
                            val_obj->device,
                            "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-"
                            "descriptorBindingStorageTexelBufferUpdateAfterBind-03010",
                            "vkCreateDescriptorSetLayout(): pBindings[%u] can't have VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT "
                            "for %s since descriptorBindingStorageTexelBufferUpdateAfterBind is not enabled.",
                            i, string_VkDescriptorType(binding_info.descriptorType));
                    }
                    if ((binding_info.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ||
                         binding_info.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                         binding_info.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)) {
                        skip |= val_obj->LogError(val_obj->device, "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-None-03011",
                                                  "vkCreateDescriptorSetLayout(): pBindings[%u] can't have "
                                                  "VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT for %s.",
                                                  i, string_VkDescriptorType(binding_info.descriptorType));
                    }

                    if (binding_info.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT &&
                        !inline_uniform_block_features->descriptorBindingInlineUniformBlockUpdateAfterBind) {
                        skip |= val_obj->LogError(
                            val_obj->device,
                            "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-"
                            "descriptorBindingInlineUniformBlockUpdateAfterBind-02211",
                            "vkCreateDescriptorSetLayout(): pBindings[%u] can't have VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT "
                            "for %s since descriptorBindingInlineUniformBlockUpdateAfterBind is not enabled.",
                            i, string_VkDescriptorType(binding_info.descriptorType));
                    }
                }

                if (flags_create_info->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT) {
                    if (!core12_features->descriptorBindingUpdateUnusedWhilePending) {
                        skip |= val_obj->LogError(
                            val_obj->device,
                            "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-descriptorBindingUpdateUnusedWhilePending-03012",
                            "vkCreateDescriptorSetLayout(): pBindings[%u] can't have "
                            "VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT for %s since "
                            "descriptorBindingUpdateUnusedWhilePending is not enabled.",
                            i, string_VkDescriptorType(binding_info.descriptorType));
                    }
                }

                if (flags_create_info->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT) {
                    if (!core12_features->descriptorBindingPartiallyBound) {
                        skip |= val_obj->LogError(
                            val_obj->device,
                            "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-descriptorBindingPartiallyBound-03013",
                            "vkCreateDescriptorSetLayout(): pBindings[%u] can't have VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT for "
                            "%s since descriptorBindingPartiallyBound is not enabled.",
                            i, string_VkDescriptorType(binding_info.descriptorType));
                    }
                }

                if (flags_create_info->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
                    if (binding_info.binding != max_binding) {
                        skip |= val_obj->LogError(
                            val_obj->device, "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-pBindingFlags-03004",
                            "vkCreateDescriptorSetLayout(): pBindings[%u] has VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT "
                            "but %u is the largest value of all the bindings.",
                            i, binding_info.binding);
                    }

                    if (!core12_features->descriptorBindingVariableDescriptorCount) {
                        skip |= val_obj->LogError(
                            val_obj->device,
                            "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-descriptorBindingVariableDescriptorCount-03014",
                            "vkCreateDescriptorSetLayout(): pBindings[%u] can't have "
                            "VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT for %s since "
                            "descriptorBindingVariableDescriptorCount is not enabled.",
                            i, string_VkDescriptorType(binding_info.descriptorType));
                    }
                    if ((binding_info.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) ||
                        (binding_info.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)) {
                        skip |= val_obj->LogError(val_obj->device,
                                                  "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-pBindingFlags-03015",
                                                  "vkCreateDescriptorSetLayout(): pBindings[%u] can't have "
                                                  "VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT for %s.",
                                                  i, string_VkDescriptorType(binding_info.descriptorType));
                    }
                }

                if (push_descriptor_set &&
                    (flags_create_info->pBindingFlags[i] &
                     (VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
                      VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT))) {
                    skip |= val_obj->LogError(
                        val_obj->device, "VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-flags-03003",
                        "vkCreateDescriptorSetLayout(): pBindings[%u] can't have VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, "
                        "VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT, or "
                        "VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT for with "
                        "VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR.",
                        i);
                }
            }
        }
    }

    if ((push_descriptor_set) && (total_descriptors > max_push_descriptors)) {
        const char *undefined = push_descriptor_ext ? "" : " -- undefined";
        skip |= val_obj->LogError(
            val_obj->device, "VUID-VkDescriptorSetLayoutCreateInfo-flags-00281",
            "vkCreateDescriptorSetLayout(): for push descriptor, total descriptor count in layout (%" PRIu64
            ") must not be greater than VkPhysicalDevicePushDescriptorPropertiesKHR::maxPushDescriptors (%" PRIu32 "%s).",
            total_descriptors, max_push_descriptors, undefined);
    }

    return skip;
}

void cvdescriptorset::AllocateDescriptorSetsData::Init(uint32_t count) {
    layout_nodes.resize(count);
}

cvdescriptorset::DescriptorSet::DescriptorSet(const VkDescriptorSet set, DESCRIPTOR_POOL_STATE *pool_state,
                                              const std::shared_ptr<DescriptorSetLayout const> &layout, uint32_t variable_count,
                                              const cvdescriptorset::DescriptorSet::StateTracker *state_data)
    : some_update_(false),
      set_(set),
      pool_state_(pool_state),
      layout_(layout),
      state_data_(state_data),
      variable_count_(variable_count),
      change_count_(0) {
    // Foreach binding, create default descriptors of given type
    descriptors_.reserve(layout_->GetTotalDescriptorCount());
    descriptor_store_.resize(layout_->GetTotalDescriptorCount());
    auto free_descriptor = descriptor_store_.data();
    for (uint32_t i = 0; i < layout_->GetBindingCount(); ++i) {
        auto type = layout_->GetTypeFromIndex(i);
        switch (type) {
            case VK_DESCRIPTOR_TYPE_SAMPLER: {
                auto immut_sampler = layout_->GetImmutableSamplerPtrFromIndex(i);
                for (uint32_t di = 0; di < layout_->GetDescriptorCountFromIndex(i); ++di) {
                    if (immut_sampler) {
                        descriptors_.emplace_back(new ((free_descriptor++)->Sampler())
                                                      SamplerDescriptor(state_data, immut_sampler + di));
                        some_update_ = true;  // Immutable samplers are updated at creation
                    } else {
                        descriptors_.emplace_back(new ((free_descriptor++)->Sampler()) SamplerDescriptor(state_data, nullptr));
                    }
                }
                break;
            }
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
                auto immut = layout_->GetImmutableSamplerPtrFromIndex(i);
                for (uint32_t di = 0; di < layout_->GetDescriptorCountFromIndex(i); ++di) {
                    if (immut) {
                        descriptors_.emplace_back(new ((free_descriptor++)->ImageSampler())
                                                      ImageSamplerDescriptor(state_data, immut + di));
                        some_update_ = true;  // Immutable samplers are updated at creation
                    } else {
                        descriptors_.emplace_back(new ((free_descriptor++)->ImageSampler())
                                                      ImageSamplerDescriptor(state_data, nullptr));
                    }
                }
                break;
            }
            // ImageDescriptors
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                for (uint32_t di = 0; di < layout_->GetDescriptorCountFromIndex(i); ++di) {
                    descriptors_.emplace_back(new ((free_descriptor++)->Image()) ImageDescriptor(type));
                }
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                for (uint32_t di = 0; di < layout_->GetDescriptorCountFromIndex(i); ++di) {
                    descriptors_.emplace_back(new ((free_descriptor++)->Texel()) TexelDescriptor(type));
                }
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                for (uint32_t di = 0; di < layout_->GetDescriptorCountFromIndex(i); ++di) {
                    descriptors_.emplace_back(new ((free_descriptor++)->Buffer()) BufferDescriptor(type));
                }
                break;
            case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT:
                for (uint32_t di = 0; di < layout_->GetDescriptorCountFromIndex(i); ++di) {
                    descriptors_.emplace_back(new ((free_descriptor++)->InlineUniform()) InlineUniformDescriptor(type));
                }
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                for (uint32_t di = 0; di < layout_->GetDescriptorCountFromIndex(i); ++di) {
                    descriptors_.emplace_back(new ((free_descriptor++)->AccelerationStructure())
                                                  AccelerationStructureDescriptor(type));
                }
                break;
            default:
                if (IsDynamicDescriptor(type) && IsBufferDescriptor(type)) {
                    for (uint32_t di = 0; di < layout_->GetDescriptorCountFromIndex(i); ++di) {
                        dynamic_offset_idx_to_descriptor_list_.push_back(descriptors_.size());
                        descriptors_.emplace_back(new ((free_descriptor++)->Buffer()) BufferDescriptor(type));
                    }
                } else {
                    assert(0);  // Bad descriptor type specified
                }
                break;
        }
    }
}

cvdescriptorset::DescriptorSet::~DescriptorSet() {}

static std::string StringDescriptorReqViewType(descriptor_req req) {
    std::string result("");
    for (unsigned i = 0; i <= VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; i++) {
        if (req & (1 << i)) {
            if (result.size()) result += ", ";
            result += string_VkImageViewType(VkImageViewType(i));
        }
    }

    if (!result.size()) result = "(none)";

    return result;
}

static char const *StringDescriptorReqComponentType(descriptor_req req) {
    if (req & DESCRIPTOR_REQ_COMPONENT_TYPE_SINT) return "SINT";
    if (req & DESCRIPTOR_REQ_COMPONENT_TYPE_UINT) return "UINT";
    if (req & DESCRIPTOR_REQ_COMPONENT_TYPE_FLOAT) return "FLOAT";
    return "(none)";
}

unsigned DescriptorRequirementsBitsFromFormat(VkFormat fmt) {
    if (FormatIsSInt(fmt)) return DESCRIPTOR_REQ_COMPONENT_TYPE_SINT;
    if (FormatIsUInt(fmt)) return DESCRIPTOR_REQ_COMPONENT_TYPE_UINT;
    if (FormatIsDepthAndStencil(fmt)) return DESCRIPTOR_REQ_COMPONENT_TYPE_FLOAT | DESCRIPTOR_REQ_COMPONENT_TYPE_UINT;
    if (fmt == VK_FORMAT_UNDEFINED) return 0;
    // everything else -- UNORM/SNORM/FLOAT/USCALED/SSCALED is all float in the shader.
    return DESCRIPTOR_REQ_COMPONENT_TYPE_FLOAT;
}

// Validate that the state of this set is appropriate for the given bindings and dynamic_offsets at Draw time
//  This includes validating that all descriptors in the given bindings are updated,
//  that any update buffers are valid, and that any dynamic offsets are within the bounds of their buffers.
// Return true if state is acceptable, or false and write an error message into error string
bool CoreChecks::ValidateDrawState(const DescriptorSet *descriptor_set, const BindingReqMap &bindings,
                                   const std::vector<uint32_t> &dynamic_offsets, const CMD_BUFFER_STATE *cb_node,
                                   const std::vector<IMAGE_VIEW_STATE *> *attachments, const std::vector<SUBPASS_INFO> &subpasses,
                                   const char *caller, const DrawDispatchVuid &vuids) const {
    bool result = false;
    VkFramebuffer framebuffer = cb_node->activeFramebuffer ? cb_node->activeFramebuffer->framebuffer : VK_NULL_HANDLE;
    for (const auto &binding_pair : bindings) {
        const auto binding = binding_pair.first;
        DescriptorSetLayout::ConstBindingIterator binding_it(descriptor_set->GetLayout().get(), binding);
        if (binding_it.AtEnd()) {  //  End at construction is the condition for an invalid binding.
            auto set = descriptor_set->GetSet();
            result |= LogError(set, vuids.descriptor_valid,
                               "%s encountered the following validation error at %s time: Attempting to "
                               "validate DrawState for binding #%u  which is an invalid binding for this descriptor set.",
                               report_data->FormatHandle(set).c_str(), caller, binding);
            return result;
        }

        if (binding_it.GetDescriptorBindingFlags() &
            (VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)) {
            // Can't validate the descriptor because it may not have been updated,
            // or the view could have been destroyed
            continue;
        }
        // // This is a record time only path
        const bool record_time_validate = true;
        result |= ValidateDescriptorSetBindingData(cb_node, descriptor_set, dynamic_offsets, binding_pair, framebuffer, attachments,
                                                   subpasses, record_time_validate, caller, vuids);
    }
    return result;
}

bool CoreChecks::ValidateDescriptorSetBindingData(const CMD_BUFFER_STATE *cb_node, const DescriptorSet *descriptor_set,
                                                  const std::vector<uint32_t> &dynamic_offsets,
                                                  const std::pair<const uint32_t, DescriptorRequirement> &binding_info,
                                                  VkFramebuffer framebuffer, const std::vector<IMAGE_VIEW_STATE *> *attachments,
                                                  const std::vector<SUBPASS_INFO> &subpasses, bool record_time_validate,
                                                  const char *caller, const DrawDispatchVuid &vuids) const {
    using DescriptorClass = cvdescriptorset::DescriptorClass;
    using BufferDescriptor = cvdescriptorset::BufferDescriptor;
    using ImageDescriptor = cvdescriptorset::ImageDescriptor;
    using ImageSamplerDescriptor = cvdescriptorset::ImageSamplerDescriptor;
    using SamplerDescriptor = cvdescriptorset::SamplerDescriptor;
    using TexelDescriptor = cvdescriptorset::TexelDescriptor;
    using AccelerationStructureDescriptor = cvdescriptorset::AccelerationStructureDescriptor;
    const auto reqs = binding_info.second.reqs;
    const auto binding = binding_info.first;
    DescriptorSetLayout::ConstBindingIterator binding_it(descriptor_set->GetLayout().get(), binding);
    {
        // Copy the range, the end range is subject to update based on variable length descriptor arrays.
        cvdescriptorset::IndexRange index_range = binding_it.GetGlobalIndexRange();
        auto array_idx = 0;  // Track array idx if we're dealing with array descriptors

        if (binding_it.IsVariableDescriptorCount()) {
            // Only validate the first N descriptors if it uses variable_count
            index_range.end = index_range.start + descriptor_set->GetVariableDescriptorCount();
        }

        for (uint32_t i = index_range.start; i < index_range.end; ++i, ++array_idx) {
            uint32_t index = i - index_range.start;
            const auto *descriptor = descriptor_set->GetDescriptorFromGlobalIndex(i);
            const auto descriptor_class = descriptor->GetClass();

            if (descriptor_class == DescriptorClass::InlineUniform) {
                // Can't validate the descriptor because it may not have been updated.
                continue;
            } else if (!descriptor->updated) {
                auto set = descriptor_set->GetSet();
                return LogError(
                    set, vuids.descriptor_valid,
                    "Descriptor set %s encountered the following validation error at %s time: Descriptor in binding #%" PRIu32
                    " index %" PRIu32
                    " is being used in draw but has never been updated via vkUpdateDescriptorSets() or a similar call.",
                    report_data->FormatHandle(set).c_str(), caller, binding, index);
            } else {
                if (descriptor_class == DescriptorClass::GeneralBuffer) {
                    // Verify that buffers are valid
                    auto buffer = static_cast<const BufferDescriptor *>(descriptor)->GetBuffer();
                    auto buffer_node = static_cast<const BufferDescriptor *>(descriptor)->GetBufferState();
                    if ((!buffer_node && !enabled_features.robustness2_features.nullDescriptor) ||
                        (buffer_node && buffer_node->destroyed)) {
                        auto set = descriptor_set->GetSet();
                        return LogError(set, vuids.descriptor_valid,
                                        "Descriptor set %s encountered the following validation error at %s time: Descriptor in "
                                        "binding #%" PRIu32 " index %" PRIu32
                                        " is using buffer %s that is invalid or has been destroyed.",
                                        report_data->FormatHandle(set).c_str(), caller, binding, index,
                                        report_data->FormatHandle(buffer).c_str());
                    }
                    if (buffer) {
                        if (!buffer_node->sparse) {
                            for (const auto *mem_binding : buffer_node->GetBoundMemory()) {
                                if (mem_binding->destroyed) {
                                    auto set = descriptor_set->GetSet();
                                    return LogError(
                                        set, vuids.descriptor_valid,
                                        "Descriptor set %s encountered the following validation error at %s time: Descriptor in "
                                        "binding #%" PRIu32 " index %" PRIu32
                                        " is uses buffer %s that references invalid memory %s.",
                                        report_data->FormatHandle(set).c_str(), caller, binding, index,
                                        report_data->FormatHandle(buffer).c_str(),
                                        report_data->FormatHandle(mem_binding->mem).c_str());
                                }
                            }
                        }
                        if (enabled_features.core11.protectedMemory == VK_TRUE) {
                            if (ValidateProtectedBuffer(cb_node, buffer_node, caller, vuids.unprotected_command_buffer,
                                                        "Buffer is in a descriptorSet")) {
                                return true;
                            }
                            if (binding_info.second.is_writable &&
                                ValidateUnprotectedBuffer(cb_node, buffer_node, caller, vuids.protected_command_buffer,
                                                          "Buffer is in a descriptorSet")) {
                                return true;
                            }
                        }
                    }
                } else if (descriptor_class == DescriptorClass::ImageSampler || descriptor_class == DescriptorClass::Image) {
                    VkImageView image_view;
                    VkImageLayout image_layout;
                    const IMAGE_VIEW_STATE *image_view_state;
                    std::vector<const SAMPLER_STATE *> sampler_states;

                    if (descriptor_class == DescriptorClass::ImageSampler) {
                        const ImageSamplerDescriptor *image_descriptor = static_cast<const ImageSamplerDescriptor *>(descriptor);
                        image_view = image_descriptor->GetImageView();
                        image_view_state = image_descriptor->GetImageViewState();
                        image_layout = image_descriptor->GetImageLayout();
                        sampler_states.emplace_back(image_descriptor->GetSamplerState());
                    } else {
                        const ImageDescriptor *image_descriptor = static_cast<const ImageDescriptor *>(descriptor);
                        image_view = image_descriptor->GetImageView();
                        image_view_state = image_descriptor->GetImageViewState();
                        image_layout = image_descriptor->GetImageLayout();

                        if (binding_info.second.samplers_used_by_image.size() > index) {
                            for (auto &sampler : binding_info.second.samplers_used_by_image[index]) {
                                // NOTE: This check _shouldn't_ be necessary due to the checks made in IsSpecificDescriptorType in
                                //       shader_validation.cpp. However, without this check some traces still crash.
                                if (sampler.second &&
                                    (sampler.second->GetClass() == cvdescriptorset::DescriptorClass::PlainSampler)) {
                                    const auto *sampler_state =
                                        static_cast<const cvdescriptorset::SamplerDescriptor *>(sampler.second)->GetSamplerState();
                                    if (sampler_state) sampler_states.emplace_back(sampler_state);
                                }
                            }
                        }
                    }

                    if ((!image_view_state && !enabled_features.robustness2_features.nullDescriptor) ||
                        (image_view_state && image_view_state->destroyed)) {
                        // Image view must have been destroyed since initial update. Could potentially flag the descriptor
                        //  as "invalid" (updated = false) at DestroyImageView() time and detect this error at bind time

                        auto set = descriptor_set->GetSet();
                        return LogError(set, vuids.descriptor_valid,
                                        "Descriptor set %s encountered the following validation error at %s time: Descriptor in "
                                        "binding #%" PRIu32 " index %" PRIu32
                                        " is using imageView %s that is invalid or has been destroyed.",
                                        report_data->FormatHandle(set).c_str(), caller, binding, index,
                                        report_data->FormatHandle(image_view).c_str());
                    }
                    if (image_view) {
                        const auto &image_view_ci = image_view_state->create_info;
                        const auto *image_state = image_view_state->image_state.get();

                        if (reqs & DESCRIPTOR_REQ_ALL_VIEW_TYPE_BITS) {
                            if (~reqs & (1 << image_view_ci.viewType)) {
                                auto set = descriptor_set->GetSet();
                                return LogError(
                                    set, vuids.descriptor_valid,
                                    "Descriptor set %s encountered the following validation error at %s time: Descriptor "
                                    "in binding #%" PRIu32 " index %" PRIu32 " requires an image view of type %s but got %s.",
                                    report_data->FormatHandle(set).c_str(), caller, binding, index,
                                    StringDescriptorReqViewType(reqs).c_str(), string_VkImageViewType(image_view_ci.viewType));
                            }

                            if (!(reqs & image_view_state->descriptor_format_bits)) {
                                // bad component type
                                auto set = descriptor_set->GetSet();
                                return LogError(set, vuids.descriptor_valid,
                                                "Descriptor set %s encountered the following validation error at %s time: "
                                                "Descriptor in binding "
                                                "#%" PRIu32 " index %" PRIu32
                                                " requires %s component type, but bound descriptor format is %s.",
                                                report_data->FormatHandle(set).c_str(), caller, binding, index,
                                                StringDescriptorReqComponentType(reqs), string_VkFormat(image_view_ci.format));
                            }
                        }

                        // NOTE: Submit time validation of UPDATE_AFTER_BIND image layout is not possible with the
                        // image layout tracking as currently implemented, so only record_time_validation is done
                        if (!disabled[image_layout_validation] && record_time_validate) {
                            auto image_node = image_view_state->image_state.get();
                            assert(image_node);
                            // Verify Image Layout
                            // No "invalid layout" VUID required for this call, since the optimal_layout parameter is UNDEFINED.
                            bool hit_error = false;
                            VerifyImageLayout(cb_node, image_node, image_view_state->normalized_subresource_range,
                                              image_view_ci.subresourceRange.aspectMask, image_layout, VK_IMAGE_LAYOUT_UNDEFINED,
                                              caller, kVUIDUndefined, "VUID-VkDescriptorImageInfo-imageLayout-00344", &hit_error);
                            if (hit_error) {
                                auto set = descriptor_set->GetSet();
                                return LogError(
                                    set, vuids.descriptor_valid,
                                    "Descriptor set %s encountered the following validation error at %s time: Image layout "
                                    "specified "
                                    "at vkUpdateDescriptorSet* or vkCmdPushDescriptorSet* time "
                                    "doesn't match actual image layout at time descriptor is used. See previous error callback for "
                                    "specific details.",
                                    report_data->FormatHandle(set).c_str(), caller);
                            }
                        }

                        // Verify Sample counts
                        if ((reqs & DESCRIPTOR_REQ_SINGLE_SAMPLE) && image_view_state->samples != VK_SAMPLE_COUNT_1_BIT) {
                            auto set = descriptor_set->GetSet();
                            return LogError(
                                set, vuids.descriptor_valid,
                                "Descriptor set %s encountered the following validation error at %s time: Descriptor in "
                                "binding #%" PRIu32 " index %" PRIu32
                                " requires bound image to have VK_SAMPLE_COUNT_1_BIT but got %s.",
                                report_data->FormatHandle(set).c_str(), caller, binding, index,
                                string_VkSampleCountFlagBits(image_view_state->samples));
                        }
                        if ((reqs & DESCRIPTOR_REQ_MULTI_SAMPLE) && image_view_state->samples == VK_SAMPLE_COUNT_1_BIT) {
                            auto set = descriptor_set->GetSet();
                            return LogError(set, vuids.descriptor_valid,
                                            "Descriptor set %s encountered the following validation error at %s time: Descriptor "
                                            "in binding #%" PRIu32 " index %" PRIu32
                                            " requires bound image to have multiple samples, but got VK_SAMPLE_COUNT_1_BIT.",
                                            report_data->FormatHandle(set).c_str(), caller, binding, index);
                        }

                        const VkDescriptorType descriptor_type = binding_it.GetType();

                        // Verify VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT
                        if ((reqs & DESCRIPTOR_REQ_VIEW_ATOMIC_OPERATION) &&
                            (descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) &&
                            !(image_view_state->format_features & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT)) {
                            auto set = descriptor_set->GetSet();
                            LogObjectList objlist(set);
                            objlist.add(image_view);
                            return LogError(objlist, vuids.imageview_atomic,
                                            "Descriptor set %s encountered the following validation error at %s time: Descriptor "
                                            "in binding #%" PRIu32 " index %" PRIu32
                                            ", %s, format %s, doesn't "
                                            "contain VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT.",
                                            report_data->FormatHandle(set).c_str(), caller, binding, index,
                                            report_data->FormatHandle(image_view).c_str(), string_VkFormat(image_view_ci.format));
                        }

                        // Verify if attachments are used in DescriptorSet
                        if (attachments && attachments->size() > 0 && (descriptor_type != VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)) {
                            bool ds_aspect = (image_view_state->create_info.subresourceRange.aspectMask &
                                              (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
                                                 ? true
                                                 : false;
                            uint32_t att_index = 0;
                            for (const auto &view_state : *attachments) {
                                if (!subpasses[att_index].used || !view_state || view_state->destroyed) {
                                    continue;
                                }
                                if (ds_aspect && subpasses[att_index].usage == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                                    if ((image_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
                                         image_layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
                                         image_layout == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL) &&
                                        (subpasses[att_index].layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
                                         subpasses[att_index].layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
                                         subpasses[att_index].layout == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL)) {
                                        continue;
                                    }
                                    if ((image_layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL &&
                                         subpasses[att_index].layout ==
                                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL) ||
                                        (subpasses[att_index].layout ==
                                             VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL &&
                                         image_layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL)) {
                                        continue;
                                    }
                                }
                                if (view_state->image_view == image_view) {
                                    auto set = descriptor_set->GetSet();
                                    LogObjectList objlist(set);
                                    objlist.add(image_view);
                                    objlist.add(framebuffer);
                                    return LogError(
                                        objlist, vuids.image_subresources,
                                        "Descriptor set %s encountered the following validation error at %s time: %s is used in "
                                        "Descriptor in binding #%" PRIu32 " index %" PRIu32 " and %s attachment # %" PRIu32 ".",
                                        report_data->FormatHandle(set).c_str(), caller,
                                        report_data->FormatHandle(image_view).c_str(), binding, index,
                                        report_data->FormatHandle(framebuffer).c_str(), att_index);
                                } else {
                                    if (image_view_state->OverlapSubresource(*view_state)) {
                                        auto set = descriptor_set->GetSet();
                                        LogObjectList objlist(set);
                                        objlist.add(image_view);
                                        objlist.add(framebuffer);
                                        objlist.add(view_state->image_view);
                                        return LogError(objlist, vuids.image_subresources,
                                                        "Descriptor set %s encountered the following validation error at %s time: "
                                                        "Image subresources of %s in "
                                                        "Descriptor in binding #%" PRIu32 " index %" PRIu32
                                                        " and %s in %s attachment # %" PRIu32 " overlap.",
                                                        report_data->FormatHandle(set).c_str(), caller,
                                                        report_data->FormatHandle(image_view).c_str(), binding, index,
                                                        report_data->FormatHandle(view_state->image_view).c_str(),
                                                        report_data->FormatHandle(framebuffer).c_str(), att_index);
                                    }
                                }
                                ++att_index;
                            }
                            if (enabled_features.core11.protectedMemory == VK_TRUE) {
                                if (ValidateProtectedImage(cb_node, image_view_state->image_state.get(), caller,
                                                           vuids.unprotected_command_buffer, "Image is in a descriptorSet")) {
                                    return true;
                                }
                                if (binding_info.second.is_writable &&
                                    ValidateUnprotectedImage(cb_node, image_view_state->image_state.get(), caller,
                                                             vuids.protected_command_buffer, "Image is in a descriptorSet")) {
                                    return true;
                                }
                            }
                        }

                        for (const auto *sampler_state : sampler_states) {
                            if (!sampler_state || sampler_state->destroyed) {
                                continue;
                            }

                            // TODO: Validate 04015 for DescriptorClass::PlainSampler
                            if ((sampler_state->createInfo.borderColor == VK_BORDER_COLOR_INT_CUSTOM_EXT ||
                                 sampler_state->createInfo.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT) &&
                                (sampler_state->customCreateInfo.format == VK_FORMAT_UNDEFINED)) {
                                if (image_view_state->create_info.format == VK_FORMAT_B4G4R4A4_UNORM_PACK16 ||
                                    image_view_state->create_info.format == VK_FORMAT_B5G6R5_UNORM_PACK16 ||
                                    image_view_state->create_info.format == VK_FORMAT_B5G5R5A1_UNORM_PACK16) {
                                    auto set = descriptor_set->GetSet();
                                    LogObjectList objlist(set);
                                    objlist.add(sampler_state->sampler);
                                    objlist.add(image_view_state->image_view);
                                    return LogError(
                                        objlist, "VUID-VkSamplerCustomBorderColorCreateInfoEXT-format-04015",
                                        "Descriptor set %s encountered the following validation error at %s time: Sampler %s in "
                                        "binding #%" PRIu32 " index %" PRIu32
                                        " has a custom border color with format = VK_FORMAT_UNDEFINED and is used to "
                                        "sample an image view %s with format %s",
                                        report_data->FormatHandle(set).c_str(), caller,
                                        report_data->FormatHandle(sampler_state->sampler).c_str(), binding, index,
                                        report_data->FormatHandle(image_view_state->image_view).c_str(),
                                        string_VkFormat(image_view_state->create_info.format));
                                }
                            }
                            VkFilter sampler_mag_filter = sampler_state->createInfo.magFilter;
                            VkFilter sampler_min_filter = sampler_state->createInfo.minFilter;
                            VkBool32 sampler_compare_enable = sampler_state->createInfo.compareEnable;
                            if ((sampler_mag_filter == VK_FILTER_LINEAR || sampler_min_filter == VK_FILTER_LINEAR) &&
                                (sampler_compare_enable == VK_FALSE) &&
                                !(image_view_state->format_features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
                                auto set = descriptor_set->GetSet();
                                LogObjectList objlist(set);
                                objlist.add(sampler_state->sampler);
                                objlist.add(image_view_state->image_view);
                                return LogError(objlist, vuids.linear_sampler,
                                                "Descriptor set %s encountered the following validation error at %s time: Sampler "
                                                "(%s) is set to use VK_FILTER_LINEAR with "
                                                "compareEnable is set to VK_TRUE, but image view's (%s) format (%s) does not "
                                                "contain VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT in its format features.",
                                                report_data->FormatHandle(set).c_str(), caller,
                                                report_data->FormatHandle(sampler_state->sampler).c_str(),
                                                report_data->FormatHandle(image_view_state->image_view).c_str(),
                                                string_VkFormat(image_view_state->create_info.format));
                            }
                            if (sampler_mag_filter == VK_FILTER_CUBIC_EXT || sampler_min_filter == VK_FILTER_CUBIC_EXT) {
                                if (!(image_view_state->format_features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_CUBIC_BIT_EXT)) {
                                    auto set = descriptor_set->GetSet();
                                    LogObjectList objlist(set);
                                    objlist.add(sampler_state->sampler);
                                    objlist.add(image_view_state->image_view);
                                    return LogError(objlist, vuids.cubic_sampler,
                                                    "Descriptor set %s encountered the following validation error at %s time: "
                                                    "Sampler (%s) is set to use VK_FILTER_CUBIC_EXT, then "
                                                    "image view's (%s) format (%s) MUST contain "
                                                    "VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_CUBIC_BIT_EXT in its format features.",
                                                    report_data->FormatHandle(set).c_str(), caller,
                                                    report_data->FormatHandle(sampler_state->sampler).c_str(),
                                                    report_data->FormatHandle(image_view_state->image_view).c_str(),
                                                    string_VkFormat(image_view_state->create_info.format));
                                }

                                if (IsExtEnabled(device_extensions.vk_ext_filter_cubic)) {
                                    const auto reduction_mode_info =
                                        LvlFindInChain<VkSamplerReductionModeCreateInfo>(sampler_state->createInfo.pNext);
                                    if (reduction_mode_info &&
                                        (reduction_mode_info->reductionMode == VK_SAMPLER_REDUCTION_MODE_MIN ||
                                         reduction_mode_info->reductionMode == VK_SAMPLER_REDUCTION_MODE_MAX) &&
                                        !image_view_state->filter_cubic_props.filterCubicMinmax) {
                                        auto set = descriptor_set->GetSet();
                                        LogObjectList objlist(set);
                                        objlist.add(sampler_state->sampler);
                                        objlist.add(image_view_state->image_view);
                                        return LogError(objlist, vuids.filter_cubic_min_max,
                                                        "Descriptor set %s encountered the following validation error at %s time: "
                                                        "Sampler (%s) is set to use VK_FILTER_CUBIC_EXT & %s, "
                                                        "but image view (%s) doesn't support filterCubicMinmax.",
                                                        report_data->FormatHandle(set).c_str(), caller,
                                                        report_data->FormatHandle(sampler_state->sampler).c_str(),
                                                        string_VkSamplerReductionMode(reduction_mode_info->reductionMode),
                                                        report_data->FormatHandle(image_view_state->image_view).c_str());
                                    }

                                    if (!image_view_state->filter_cubic_props.filterCubic) {
                                        auto set = descriptor_set->GetSet();
                                        LogObjectList objlist(set);
                                        objlist.add(sampler_state->sampler);
                                        objlist.add(image_view_state->image_view);
                                        return LogError(objlist, vuids.filter_cubic,
                                                        "Descriptor set %s encountered the following validation error at %s time: "
                                                        "Sampler (%s) is set to use VK_FILTER_CUBIC_EXT, "
                                                        "but image view (%s) doesn't support filterCubic.",
                                                        report_data->FormatHandle(set).c_str(), caller,
                                                        report_data->FormatHandle(sampler_state->sampler).c_str(),
                                                        report_data->FormatHandle(image_view_state->image_view).c_str());
                                    }
                                }

                                if (IsExtEnabled(device_extensions.vk_img_filter_cubic)) {
                                    if (image_view_state->create_info.viewType &
                                        (VK_IMAGE_VIEW_TYPE_3D | VK_IMAGE_VIEW_TYPE_CUBE | VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
                                        auto set = descriptor_set->GetSet();
                                        LogObjectList objlist(set);
                                        objlist.add(sampler_state->sampler);
                                        objlist.add(image_view_state->image_view);
                                        return LogError(
                                            objlist, vuids.img_filter_cubic,
                                            "Descriptor set %s encountered the following validation error at %s time: Sampler "
                                            "(%s)is set to use VK_FILTER_CUBIC_EXT while the VK_IMG_filter_cubic extension "
                                            "is enabled, but image view (%s) has an invalid imageViewType (%s).",
                                            report_data->FormatHandle(set).c_str(), caller,
                                            report_data->FormatHandle(sampler_state->sampler).c_str(),
                                            report_data->FormatHandle(image_view_state->image_view).c_str(),
                                            string_VkImageViewType(image_view_state->create_info.viewType));
                                    }
                                }
                            }

                            if ((image_state->createInfo.flags & VK_IMAGE_CREATE_CORNER_SAMPLED_BIT_NV) &&
                                (sampler_state->createInfo.addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE ||
                                 sampler_state->createInfo.addressModeV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE ||
                                 sampler_state->createInfo.addressModeW != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)) {
                                std::string address_mode_letter =
                                    (sampler_state->createInfo.addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
                                        ? "U"
                                        : (sampler_state->createInfo.addressModeV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE) ? "V"
                                                                                                                            : "W";
                                VkSamplerAddressMode address_mode =
                                    (sampler_state->createInfo.addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
                                        ? sampler_state->createInfo.addressModeU
                                        : (sampler_state->createInfo.addressModeV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)
                                              ? sampler_state->createInfo.addressModeV
                                              : sampler_state->createInfo.addressModeW;
                                auto set = descriptor_set->GetSet();
                                LogObjectList objlist(set);
                                objlist.add(sampler_state->sampler);
                                objlist.add(image_state->image);
                                objlist.add(image_view_state->image_view);
                                return LogError(objlist, vuids.corner_sampled_address_mode,
                                                "Descriptor set %s encountered the following validation error at %s time: Image "
                                                "(%s) in image view (%s) is created with flag "
                                                "VK_IMAGE_CREATE_CORNER_SAMPLED_BIT_NV and can only be sampled using "
                                                "VK_SAMPLER_ADDRESS_MODE_CLAMP_EDGE, but sampler (%s) has "
                                                "createInfo.addressMode%s set to %s.",
                                                report_data->FormatHandle(set).c_str(), caller,
                                                report_data->FormatHandle(image_state->image).c_str(),
                                                report_data->FormatHandle(image_view_state->image_view).c_str(),
                                                report_data->FormatHandle(sampler_state->sampler).c_str(),
                                                address_mode_letter.c_str(), string_VkSamplerAddressMode(address_mode));
                            }

                            // UnnormalizedCoordinates sampler validations
                            if (sampler_state->createInfo.unnormalizedCoordinates) {
                                // If ImageView is used by a unnormalizedCoordinates sampler, it needs to check ImageView type
                                if (image_view_ci.viewType == VK_IMAGE_VIEW_TYPE_3D ||
                                    image_view_ci.viewType == VK_IMAGE_VIEW_TYPE_CUBE ||
                                    image_view_ci.viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
                                    image_view_ci.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY ||
                                    image_view_ci.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
                                    auto set = descriptor_set->GetSet();
                                    LogObjectList objlist(set);
                                    objlist.add(image_view);
                                    objlist.add(sampler_state->sampler);
                                    return LogError(
                                        objlist, vuids.sampler_imageview_type,
                                        "Descriptor set %s encountered the following validation error at %s time: %s, type: %s in "
                                        "Descriptor in binding #%" PRIu32 " index %" PRIu32 "is used by %s.",
                                        report_data->FormatHandle(set).c_str(), caller,
                                        report_data->FormatHandle(image_view).c_str(),
                                        string_VkImageViewType(image_view_ci.viewType), binding, index,
                                        report_data->FormatHandle(sampler_state->sampler).c_str());
                                }

                                // sampler must not be used with any of the SPIR-V OpImageSample* or OpImageSparseSample*
                                // instructions with ImplicitLod, Dref or Proj in their name
                                if (reqs & DESCRIPTOR_REQ_SAMPLER_IMPLICITLOD_DREF_PROJ) {
                                    auto set = descriptor_set->GetSet();
                                    LogObjectList objlist(set);
                                    objlist.add(image_view);
                                    objlist.add(sampler_state->sampler);
                                    return LogError(
                                        objlist, vuids.sampler_implicitLod_dref_proj,
                                        "Descriptor set %s encountered the following validation error at %s time: %s in "
                                        "Descriptor in binding #%" PRIu32 " index %" PRIu32
                                        " is used by %s that uses invalid operator.",
                                        report_data->FormatHandle(set).c_str(), caller,
                                        report_data->FormatHandle(image_view).c_str(), binding, index,
                                        report_data->FormatHandle(sampler_state->sampler).c_str());
                                }

                                // sampler must not be used with any of the SPIR-V OpImageSample* or OpImageSparseSample*
                                // instructions that includes a LOD bias or any offset values
                                if (reqs & DESCRIPTOR_REQ_SAMPLER_BIAS_OFFSET) {
                                    auto set = descriptor_set->GetSet();
                                    LogObjectList objlist(set);
                                    objlist.add(image_view);
                                    objlist.add(sampler_state->sampler);
                                    return LogError(
                                        objlist, vuids.sampler_bias_offset,
                                        "Descriptor set %s encountered the following validation error at %s time: %s in "
                                        "Descriptor in binding #%" PRIu32 " index %" PRIu32
                                        " is used by %s that uses invalid bias or offset operator.",
                                        report_data->FormatHandle(set).c_str(), caller,
                                        report_data->FormatHandle(image_view).c_str(), binding, index,
                                        report_data->FormatHandle(sampler_state->sampler).c_str());
                                }
                            }
                        }
                    }
                } else if (descriptor_class == DescriptorClass::TexelBuffer) {
                    auto texel_buffer = static_cast<const TexelDescriptor *>(descriptor);
                    auto buffer_view = texel_buffer->GetBufferView();
                    auto buffer_view_state = texel_buffer->GetBufferViewState();

                    if ((!buffer_view_state && !enabled_features.robustness2_features.nullDescriptor) ||
                        (buffer_view_state && buffer_view_state->destroyed)) {
                        auto set = descriptor_set->GetSet();
                        return LogError(set, vuids.descriptor_valid,
                                        "Descriptor set %s encountered the following validation error at %s time: Descriptor in "
                                        "binding #%" PRIu32 " index %" PRIu32
                                        " is using bufferView %s that is invalid or has been destroyed.",
                                        report_data->FormatHandle(set).c_str(), caller, binding, index,
                                        report_data->FormatHandle(buffer_view).c_str());
                    }
                    if (buffer_view) {
                        auto buffer = buffer_view_state->create_info.buffer;
                        auto buffer_state = buffer_view_state->buffer_state.get();
                        if (buffer_state->destroyed) {
                            auto set = descriptor_set->GetSet();
                            return LogError(
                                set, vuids.descriptor_valid,
                                "Descriptor set %s encountered the following validation error at %s time: Descriptor in "
                                "binding #%" PRIu32 " index %" PRIu32 " is using buffer %s that has been destroyed.",
                                report_data->FormatHandle(set).c_str(), caller, binding, index,
                                report_data->FormatHandle(buffer).c_str());
                        }
                        auto format_bits = DescriptorRequirementsBitsFromFormat(buffer_view_state->create_info.format);

                        if (!(reqs & format_bits)) {
                            // bad component type
                            auto set = descriptor_set->GetSet();
                            return LogError(
                                set, vuids.descriptor_valid,
                                "Descriptor set %s encountered the following validation error at %s time: Descriptor in "
                                "binding #%" PRIu32 " index %" PRIu32
                                " requires %s component type, but bound descriptor format is %s.",
                                report_data->FormatHandle(set).c_str(), caller, binding, index,
                                StringDescriptorReqComponentType(reqs), string_VkFormat(buffer_view_state->create_info.format));
                        }

                        // Verify VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT
                        if ((reqs & DESCRIPTOR_REQ_VIEW_ATOMIC_OPERATION) &&
                            (descriptor_set->GetTypeFromBinding(binding) == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) &&
                            !(buffer_view_state->format_features & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT)) {
                            auto set = descriptor_set->GetSet();
                            LogObjectList objlist(set);
                            objlist.add(buffer_view);
                            return LogError(objlist, "UNASSIGNED-None-MismatchAtomicBufferFeature",
                                            "Descriptor set %s encountered the following validation error at %s time: Descriptor "
                                            "in binding #%" PRIu32 " index %" PRIu32
                                            ", %s, format %s, doesn't "
                                            "contain VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT.",
                                            report_data->FormatHandle(set).c_str(), caller, binding, index,
                                            report_data->FormatHandle(buffer_view).c_str(),
                                            string_VkFormat(buffer_view_state->create_info.format));
                        }
                        if (enabled_features.core11.protectedMemory == VK_TRUE) {
                            if (ValidateProtectedBuffer(cb_node, buffer_view_state->buffer_state.get(), caller,
                                                        vuids.unprotected_command_buffer, "Buffer is in a descriptorSet")) {
                                return true;
                            }
                            if (binding_info.second.is_writable &&
                                ValidateUnprotectedBuffer(cb_node, buffer_view_state->buffer_state.get(), caller,
                                                          vuids.protected_command_buffer, "Buffer is in a descriptorSet")) {
                                return true;
                            }
                        }
                    }
                } else if (descriptor_class == DescriptorClass::AccelerationStructure) {
                    // Verify that acceleration structures are valid
                    bool is_khr = (*((cvdescriptorset::AccelerationStructureDescriptor *)(descriptor))).is_khr();
                    if (is_khr) {
                        auto acc = static_cast<const AccelerationStructureDescriptor *>(descriptor)->GetAccelerationStructure();
                        auto acc_node =
                            static_cast<const AccelerationStructureDescriptor *>(descriptor)->GetAccelerationStructureStateKHR();
                        if (!acc_node || acc_node->destroyed) {
                            if (acc != VK_NULL_HANDLE || !enabled_features.robustness2_features.nullDescriptor) {
                                auto set = descriptor_set->GetSet();
                                return LogError(set, vuids.descriptor_valid,
                                                "Descriptor set %s encountered the following validation error at %s time: "
                                                "Descriptor in binding #%" PRIu32 " index %" PRIu32
                                                " is using acceleration structure %s that is invalid or has been destroyed.",
                                                report_data->FormatHandle(set).c_str(), caller, binding, index,
                                                report_data->FormatHandle(acc).c_str());
                            }
                        } else {
                            for (const auto *mem_binding : acc_node->GetBoundMemory()) {
                                if (mem_binding->destroyed) {
                                    auto set = descriptor_set->GetSet();
                                    return LogError(
                                        set, vuids.descriptor_valid,
                                        "Descriptor set %s encountered the following validation error at %s time: Descriptor in "
                                        "binding #%" PRIu32 " index %" PRIu32
                                        " is using acceleration structure %s that references invalid memory %s.",
                                        report_data->FormatHandle(set).c_str(), caller, binding, index,
                                        report_data->FormatHandle(acc).c_str(),
                                        report_data->FormatHandle(mem_binding->mem).c_str());
                                }
                            }
                        }
                    } else {
                        auto acc = static_cast<const AccelerationStructureDescriptor *>(descriptor)->GetAccelerationStructureNV();
                        auto acc_node =
                            static_cast<const AccelerationStructureDescriptor *>(descriptor)->GetAccelerationStructureStateNV();
                        if (!acc_node || acc_node->destroyed) {
                            if (acc != VK_NULL_HANDLE || !enabled_features.robustness2_features.nullDescriptor) {
                                auto set = descriptor_set->GetSet();
                                return LogError(set, vuids.descriptor_valid,
                                                "Descriptor set %s encountered the following validation error at %s time: "
                                                "Descriptor in binding #%" PRIu32 " index %" PRIu32
                                                " is using acceleration structure %s that is invalid or has been destroyed.",
                                                report_data->FormatHandle(set).c_str(), caller, binding, index,
                                                report_data->FormatHandle(acc).c_str());
                            }
                        } else {
                            for (const auto *mem_binding : acc_node->GetBoundMemory()) {
                                if (mem_binding->destroyed) {
                                    auto set = descriptor_set->GetSet();
                                    return LogError(
                                        set, vuids.descriptor_valid,
                                        "Descriptor set %s encountered the following validation error at %s time: Descriptor in "
                                        "binding #%" PRIu32 " index %" PRIu32
                                        " is using acceleration structure %s that references invalid memory %s.",
                                        report_data->FormatHandle(set).c_str(), caller, binding, index,
                                        report_data->FormatHandle(acc).c_str(),
                                        report_data->FormatHandle(mem_binding->mem).c_str());
                                }
                            }
                        }
                    }
                }

                // If the validation is related to both of image and sampler,
                // please leave it in (descriptor_class == DescriptorClass::ImageSampler || descriptor_class ==
                // DescriptorClass::Image) Here is to validate for only sampler.
                if (descriptor_class == DescriptorClass::ImageSampler || descriptor_class == DescriptorClass::PlainSampler) {
                    // Verify Sampler still valid
                    VkSampler sampler;
                    const SAMPLER_STATE *sampler_state;
                    if (descriptor_class == DescriptorClass::ImageSampler) {
                        sampler = static_cast<const ImageSamplerDescriptor *>(descriptor)->GetSampler();
                        sampler_state = static_cast<const ImageSamplerDescriptor *>(descriptor)->GetSamplerState();
                    } else {
                        sampler = static_cast<const SamplerDescriptor *>(descriptor)->GetSampler();
                        sampler_state = static_cast<const SamplerDescriptor *>(descriptor)->GetSamplerState();
                    }
                    if (!sampler_state || sampler_state->destroyed) {
                        auto set = descriptor_set->GetSet();
                        return LogError(set, vuids.descriptor_valid,
                                        "Descriptor set %s encountered the following validation error at %s time: Descriptor in "
                                        "binding #%" PRIu32 " index %" PRIu32
                                        " is using sampler %s that is invalid or has been destroyed.",
                                        report_data->FormatHandle(set).c_str(), caller, binding, index,
                                        report_data->FormatHandle(sampler).c_str());
                    } else {
                        if (sampler_state->samplerConversion && !descriptor->IsImmutableSampler()) {
                            auto set = descriptor_set->GetSet();
                            return LogError(set, vuids.descriptor_valid,
                                            "Descriptor set %s encountered the following validation error at %s time: sampler (%s) "
                                            "in the descriptor set (%s) contains a YCBCR conversion (%s), then the sampler MUST "
                                            "also exist as an immutable sampler.",
                                            report_data->FormatHandle(set).c_str(), caller,
                                            report_data->FormatHandle(sampler).c_str(),
                                            report_data->FormatHandle(descriptor_set->GetSet()).c_str(),
                                            report_data->FormatHandle(sampler_state->samplerConversion).c_str());
                        }
                    }
                }
            }
        }
    }
    return false;
}

// Set is being deleted or updates so invalidate all bound cmd buffers
void cvdescriptorset::DescriptorSet::InvalidateBoundCmdBuffers(ValidationStateTracker *state_data) {
    state_data->InvalidateCommandBuffers(cb_bindings, VulkanTypedHandle(set_, kVulkanObjectTypeDescriptorSet), /*unlink*/ false);
}

// Loop through the write updates to do for a push descriptor set, ignoring dstSet
void cvdescriptorset::DescriptorSet::PerformPushDescriptorsUpdate(ValidationStateTracker *dev_data, uint32_t write_count,
                                                                  const VkWriteDescriptorSet *p_wds) {
    assert(IsPushDescriptor());
    for (uint32_t i = 0; i < write_count; i++) {
        PerformWriteUpdate(dev_data, &p_wds[i]);
    }

    push_descriptor_set_writes.clear();
    push_descriptor_set_writes.reserve(static_cast<std::size_t>(write_count));
    for (uint32_t i = 0; i < write_count; i++) {
        push_descriptor_set_writes.push_back(safe_VkWriteDescriptorSet(&p_wds[i]));
    }
}

// Perform write update in given update struct
void cvdescriptorset::DescriptorSet::PerformWriteUpdate(ValidationStateTracker *dev_data, const VkWriteDescriptorSet *update) {
    // Perform update on a per-binding basis as consecutive updates roll over to next binding
    auto descriptors_remaining = update->descriptorCount;
    auto offset = update->dstArrayElement;
    auto orig_binding = DescriptorSetLayout::ConstBindingIterator(layout_.get(), update->dstBinding);
    auto current_binding = orig_binding;

    uint32_t update_index = 0;
    // Verify next consecutive binding matches type, stage flags & immutable sampler use and if AtEnd
    while (descriptors_remaining && orig_binding.IsConsistent(current_binding)) {
        const auto &index_range = current_binding.GetGlobalIndexRange();
        auto global_idx = index_range.start + offset;
        // global_idx is which descriptor is needed to update. If global_idx > index_range.end, it means the descriptor isn't in
        // this binding, maybe in next binding.
        if (global_idx >= index_range.end) {
            offset -= current_binding.GetDescriptorCount();
            ++current_binding;
            continue;
        }

        // Loop over the updates for a single binding at a time
        uint32_t update_count = std::min(descriptors_remaining, current_binding.GetDescriptorCount() - offset);
        for (uint32_t di = 0; di < update_count; ++di, ++update_index) {
            descriptors_[global_idx + di]->WriteUpdate(state_data_, update, update_index);
        }
        // Roll over to next binding in case of consecutive update
        descriptors_remaining -= update_count;
        if (descriptors_remaining) {
            // Starting offset is beyond the current binding. Check consistency, update counters and advance to the next binding,
            // looking for the start point. All bindings (even those skipped) must be consistent with the update and with the
            // original binding.
            offset = 0;
            ++current_binding;
        }
    }
    if (update->descriptorCount) {
        some_update_ = true;
        change_count_++;
    }

    if (!(layout_->GetDescriptorBindingFlagsFromBinding(update->dstBinding) &
          (VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT))) {
        InvalidateBoundCmdBuffers(dev_data);
    }
}
// Validate Copy update
bool CoreChecks::ValidateCopyUpdate(const VkCopyDescriptorSet *update, const DescriptorSet *dst_set, const DescriptorSet *src_set,
                                    const char *func_name, std::string *error_code, std::string *error_msg) const {
    auto dst_layout = dst_set->GetLayout().get();
    auto src_layout = src_set->GetLayout().get();

    // Verify dst layout still valid
    if (dst_layout->destroyed) {
        *error_code = "VUID-VkCopyDescriptorSet-dstSet-parameter";
        std::ostringstream str;
        str << "Cannot call " << func_name << " to perform copy update on dstSet " << report_data->FormatHandle(dst_set->GetSet())
            << " created with destroyed " << report_data->FormatHandle(dst_layout->GetDescriptorSetLayout()) << ".";
        *error_msg = str.str();
        return false;
    }

    // Verify src layout still valid
    if (src_layout->destroyed) {
        *error_code = "VUID-VkCopyDescriptorSet-srcSet-parameter";
        std::ostringstream str;
        str << "Cannot call " << func_name << " to perform copy update on dstSet " << report_data->FormatHandle(dst_set->GetSet())
            << " from srcSet " << report_data->FormatHandle(src_set->GetSet()) << " created with destroyed "
            << report_data->FormatHandle(src_layout->GetDescriptorSetLayout()) << ".";
        *error_msg = str.str();
        return false;
    }

    if (!dst_layout->HasBinding(update->dstBinding)) {
        *error_code = "VUID-VkCopyDescriptorSet-dstBinding-00347";
        std::stringstream error_str;
        error_str << "DescriptorSet " << report_data->FormatHandle(dst_set->GetSet())
                  << " does not have copy update dest binding of " << update->dstBinding;
        *error_msg = error_str.str();
        return false;
    }
    if (!src_set->HasBinding(update->srcBinding)) {
        *error_code = "VUID-VkCopyDescriptorSet-srcBinding-00345";
        std::stringstream error_str;
        error_str << "DescriptorSet " << report_data->FormatHandle(src_set->GetSet())
                  << " does not have copy update src binding of " << update->srcBinding;
        *error_msg = error_str.str();
        return false;
    }
    // Verify idle ds
    if (dst_set->in_use.load() &&
        !(dst_layout->GetDescriptorBindingFlagsFromBinding(update->dstBinding) &
          (VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT))) {
        // TODO : Re-using Free Idle error code, need copy update idle error code
        *error_code = "VUID-vkFreeDescriptorSets-pDescriptorSets-00309";
        std::stringstream error_str;
        error_str << "Cannot call " << func_name << " to perform copy update on descriptor set "
                  << report_data->FormatHandle(dst_set->GetSet()) << " that is in use by a command buffer";
        *error_msg = error_str.str();
        return false;
    }
    // src & dst set bindings are valid
    // Check bounds of src & dst
    auto src_start_idx = src_set->GetGlobalIndexRangeFromBinding(update->srcBinding).start + update->srcArrayElement;
    if ((src_start_idx + update->descriptorCount) > src_set->GetTotalDescriptorCount()) {
        // SRC update out of bounds
        *error_code = "VUID-VkCopyDescriptorSet-srcArrayElement-00346";
        std::stringstream error_str;
        error_str << "Attempting copy update from descriptorSet " << report_data->FormatHandle(update->srcSet) << " binding#"
                  << update->srcBinding << " with offset index of "
                  << src_set->GetGlobalIndexRangeFromBinding(update->srcBinding).start << " plus update array offset of "
                  << update->srcArrayElement << " and update of " << update->descriptorCount
                  << " descriptors oversteps total number of descriptors in set: " << src_set->GetTotalDescriptorCount();
        *error_msg = error_str.str();
        return false;
    }
    auto dst_start_idx = dst_layout->GetGlobalIndexRangeFromBinding(update->dstBinding).start + update->dstArrayElement;
    if ((dst_start_idx + update->descriptorCount) > dst_layout->GetTotalDescriptorCount()) {
        // DST update out of bounds
        *error_code = "VUID-VkCopyDescriptorSet-dstArrayElement-00348";
        std::stringstream error_str;
        error_str << "Attempting copy update to descriptorSet " << report_data->FormatHandle(dst_set->GetSet()) << " binding#"
                  << update->dstBinding << " with offset index of "
                  << dst_layout->GetGlobalIndexRangeFromBinding(update->dstBinding).start << " plus update array offset of "
                  << update->dstArrayElement << " and update of " << update->descriptorCount
                  << " descriptors oversteps total number of descriptors in set: " << dst_layout->GetTotalDescriptorCount();
        *error_msg = error_str.str();
        return false;
    }
    // Check that types match
    // TODO : Base default error case going from here is "VUID-VkAcquireNextImageInfoKHR-semaphore-parameter" 2ba which covers all
    // consistency issues, need more fine-grained error codes
    *error_code = "VUID-VkCopyDescriptorSet-srcSet-00349";
    auto src_type = src_set->GetTypeFromBinding(update->srcBinding);
    auto dst_type = dst_layout->GetTypeFromBinding(update->dstBinding);
    if (src_type != dst_type) {
        *error_code = "VUID-VkCopyDescriptorSet-dstBinding-02632";
        std::stringstream error_str;
        error_str << "Attempting copy update to descriptorSet " << report_data->FormatHandle(dst_set->GetSet()) << " binding #"
                  << update->dstBinding << " with type " << string_VkDescriptorType(dst_type) << " from descriptorSet "
                  << report_data->FormatHandle(src_set->GetSet()) << " binding #" << update->srcBinding << " with type "
                  << string_VkDescriptorType(src_type) << ". Types do not match";
        *error_msg = error_str.str();
        return false;
    }
    // Verify consistency of src & dst bindings if update crosses binding boundaries
    if ((!VerifyUpdateConsistency(report_data, DescriptorSetLayout::ConstBindingIterator(src_layout, update->srcBinding),
                                  update->srcArrayElement, update->descriptorCount, "copy update from", src_set->GetSet(),
                                  error_msg)) ||
        (!VerifyUpdateConsistency(report_data, DescriptorSetLayout::ConstBindingIterator(dst_layout, update->dstBinding),
                                  update->dstArrayElement, update->descriptorCount, "copy update to", dst_set->GetSet(),
                                  error_msg))) {
        return false;
    }

    if ((src_layout->GetCreateFlags() & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) &&
        !(dst_layout->GetCreateFlags() & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT)) {
        *error_code = "VUID-VkCopyDescriptorSet-srcSet-01918";
        std::stringstream error_str;
        error_str << "If pname:srcSet's (" << report_data->FormatHandle(update->srcSet)
                  << ") layout was created with the "
                     "ename:VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT flag "
                     "set, then pname:dstSet's ("
                  << report_data->FormatHandle(update->dstSet)
                  << ") layout must: also have been created with the "
                     "ename:VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT flag set";
        *error_msg = error_str.str();
        return false;
    }

    if (!(src_layout->GetCreateFlags() & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) &&
        (dst_layout->GetCreateFlags() & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT)) {
        *error_code = "VUID-VkCopyDescriptorSet-srcSet-01919";
        std::stringstream error_str;
        error_str << "If pname:srcSet's (" << report_data->FormatHandle(update->srcSet)
                  << ") layout was created without the "
                     "ename:VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT flag "
                     "set, then pname:dstSet's ("
                  << report_data->FormatHandle(update->dstSet)
                  << ") layout must: also have been created without the "
                     "ename:VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT flag set";
        *error_msg = error_str.str();
        return false;
    }

    if ((src_set->GetPoolState()->createInfo.flags & VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT) &&
        !(dst_set->GetPoolState()->createInfo.flags & VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT)) {
        *error_code = "VUID-VkCopyDescriptorSet-srcSet-01920";
        std::stringstream error_str;
        error_str << "If the descriptor pool from which pname:srcSet (" << report_data->FormatHandle(update->srcSet)
                  << ") was allocated was created "
                     "with the ename:VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT flag "
                     "set, then the descriptor pool from which pname:dstSet ("
                  << report_data->FormatHandle(update->dstSet)
                  << ") was allocated must: "
                     "also have been created with the ename:VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT flag set";
        *error_msg = error_str.str();
        return false;
    }

    if (!(src_set->GetPoolState()->createInfo.flags & VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT) &&
        (dst_set->GetPoolState()->createInfo.flags & VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT)) {
        *error_code = "VUID-VkCopyDescriptorSet-srcSet-01921";
        std::stringstream error_str;
        error_str << "If the descriptor pool from which pname:srcSet (" << report_data->FormatHandle(update->srcSet)
                  << ") was allocated was created "
                     "without the ename:VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT flag "
                     "set, then the descriptor pool from which pname:dstSet ("
                  << report_data->FormatHandle(update->dstSet)
                  << ") was allocated must: "
                     "also have been created without the ename:VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT flag set";
        *error_msg = error_str.str();
        return false;
    }

    if (src_type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT) {
        if ((update->srcArrayElement % 4) != 0) {
            *error_code = "VUID-VkCopyDescriptorSet-srcBinding-02223";
            std::stringstream error_str;
            error_str << "Attempting copy update to VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT binding with "
                      << "srcArrayElement " << update->srcArrayElement << " not a multiple of 4";
            *error_msg = error_str.str();
            return false;
        }
        if ((update->dstArrayElement % 4) != 0) {
            *error_code = "VUID-VkCopyDescriptorSet-dstBinding-02224";
            std::stringstream error_str;
            error_str << "Attempting copy update to VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT binding with "
                      << "dstArrayElement " << update->dstArrayElement << " not a multiple of 4";
            *error_msg = error_str.str();
            return false;
        }
        if ((update->descriptorCount % 4) != 0) {
            *error_code = "VUID-VkCopyDescriptorSet-srcBinding-02225";
            std::stringstream error_str;
            error_str << "Attempting copy update to VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT binding with "
                      << "descriptorCount " << update->descriptorCount << " not a multiple of 4";
            *error_msg = error_str.str();
            return false;
        }
    }

    // Update parameters all look good and descriptor updated so verify update contents
    if (!VerifyCopyUpdateContents(update, src_set, src_type, src_start_idx, dst_set, dst_type, dst_start_idx, func_name, error_code,
                                  error_msg)) {
        return false;
    }

    // All checks passed so update is good
    return true;
}
// Perform Copy update
void cvdescriptorset::DescriptorSet::PerformCopyUpdate(ValidationStateTracker *dev_data, const VkCopyDescriptorSet *update,
                                                       const DescriptorSet *src_set) {
    auto src_start_idx = src_set->GetGlobalIndexRangeFromBinding(update->srcBinding).start + update->srcArrayElement;
    auto dst_start_idx = layout_->GetGlobalIndexRangeFromBinding(update->dstBinding).start + update->dstArrayElement;
    // Update parameters all look good so perform update
    for (uint32_t di = 0; di < update->descriptorCount; ++di) {
        auto src = src_set->descriptors_[src_start_idx + di].get();
        auto dst = descriptors_[dst_start_idx + di].get();
        if (src->updated) {
            dst->CopyUpdate(state_data_, src);
            some_update_ = true;
            change_count_++;
        } else {
            dst->updated = false;
        }
    }

    if (!(layout_->GetDescriptorBindingFlagsFromBinding(update->dstBinding) &
          (VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT))) {
        InvalidateBoundCmdBuffers(dev_data);
    }
}

// Update the drawing state for the affected descriptors.
// Set cb_node to this set and this set to cb_node.
// Add the bindings of the descriptor
// Set the layout based on the current descriptor layout (will mask subsequent layer mismatch errors)
// TODO: Modify the UpdateDrawState virtural functions to *only* set initial layout and not change layouts
// Prereq: This should be called for a set that has been confirmed to be active for the given cb_node, meaning it's going
//   to be used in a draw by the given cb_node
void cvdescriptorset::DescriptorSet::UpdateDrawState(ValidationStateTracker *device_data, CMD_BUFFER_STATE *cb_node,
                                                     CMD_TYPE cmd_type, const PIPELINE_STATE *pipe,
                                                     const BindingReqMap &binding_req_map, const char *function) {
    if (!device_data->disabled[command_buffer_state] && !IsPushDescriptor()) {
        // bind cb to this descriptor set
        // Add bindings for descriptor set, the set's pool, and individual objects in the set
        if (device_data->AddCommandBufferBinding(cb_bindings, VulkanTypedHandle(set_, kVulkanObjectTypeDescriptorSet, this),
                                                 cb_node)) {
            device_data->AddCommandBufferBinding(pool_state_->cb_bindings,
                                                 VulkanTypedHandle(pool_state_->pool, kVulkanObjectTypeDescriptorPool, pool_state_),
                                                 cb_node);
        }
    }

    // Descriptor UpdateDrawState functions do two things - associate resources to the command buffer,
    // and call image layout validation callbacks. If both are disabled, skip the entire loop.
    if (device_data->disabled[command_buffer_state] && device_data->disabled[image_layout_validation]) {
        return;
    }

    // For the active slots, use set# to look up descriptorSet from boundDescriptorSets, and bind all of that descriptor set's
    // resources
    CMD_BUFFER_STATE::CmdDrawDispatchInfo cmd_info = {};
    for (const auto &binding_req_pair : binding_req_map) {
        auto index = layout_->GetIndexFromBinding(binding_req_pair.first);

        // We aren't validating descriptors created with PARTIALLY_BOUND or UPDATE_AFTER_BIND, so don't record state
        auto flags = layout_->GetDescriptorBindingFlagsFromIndex(index);
        if (flags & (VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)) {
            if (!(flags & VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT)) {
                cmd_info.binding_infos.emplace_back(binding_req_pair);
            }
            continue;
        }
        auto range = layout_->GetGlobalIndexRangeFromIndex(index);
        for (uint32_t i = range.start; i < range.end; ++i) {
            descriptors_[i]->UpdateDrawState(device_data, cb_node);
        }
    }

    if (cmd_info.binding_infos.size() > 0) {
        cmd_info.cmd_type = cmd_type;
        cmd_info.function = function;
        if (cb_node->activeFramebuffer) {
            cmd_info.framebuffer = cb_node->activeFramebuffer->framebuffer;
            cmd_info.attachments = cb_node->active_attachments;
            cmd_info.subpasses = cb_node->active_subpasses;
        }
        cb_node->validate_descriptorsets_in_queuesubmit[set_].emplace_back(cmd_info);
    }
}

void cvdescriptorset::DescriptorSet::FilterOneBindingReq(const BindingReqMap::value_type &binding_req_pair, BindingReqMap *out_req,
                                                         const TrackedBindings &bindings, uint32_t limit) {
    if (bindings.size() < limit) {
        const auto it = bindings.find(binding_req_pair.first);
        if (it == bindings.cend()) out_req->emplace(binding_req_pair);
    }
}

void cvdescriptorset::DescriptorSet::FilterBindingReqs(const CMD_BUFFER_STATE &cb_state, const PIPELINE_STATE &pipeline,
                                                       const BindingReqMap &in_req, BindingReqMap *out_req) const {
    // For const cleanliness we have to find in the maps...
    const auto validated_it = cached_validation_.find(&cb_state);
    if (validated_it == cached_validation_.cend()) {
        // We have nothing validated, copy in to out
        for (const auto &binding_req_pair : in_req) {
            out_req->emplace(binding_req_pair);
        }
        return;
    }
    const auto &validated = validated_it->second;

    const auto image_sample_version_it = validated.image_samplers.find(&pipeline);
    const VersionedBindings *image_sample_version = nullptr;
    if (image_sample_version_it != validated.image_samplers.cend()) {
        image_sample_version = &(image_sample_version_it->second);
    }
    const auto &dynamic_buffers = validated.dynamic_buffers;
    const auto &non_dynamic_buffers = validated.non_dynamic_buffers;
    const auto &stats = layout_->GetBindingTypeStats();
    for (const auto &binding_req_pair : in_req) {
        auto binding = binding_req_pair.first;
        VkDescriptorSetLayoutBinding const *layout_binding = layout_->GetDescriptorSetLayoutBindingPtrFromBinding(binding);
        if (!layout_binding) {
            continue;
        }
        // Caching criteria differs per type.
        // If image_layout have changed , the image descriptors need to be validated against them.
        if (IsBufferDescriptor(layout_binding->descriptorType)) {
            if (IsDynamicDescriptor(layout_binding->descriptorType)) {
                FilterOneBindingReq(binding_req_pair, out_req, dynamic_buffers, stats.dynamic_buffer_count);
            } else {
                FilterOneBindingReq(binding_req_pair, out_req, non_dynamic_buffers, stats.non_dynamic_buffer_count);
            }
        } else {
            // This is rather crude, as the changed layouts may not impact the bound descriptors,
            // but the simple "versioning" is a simple "dirt" test.
            bool stale = true;
            if (image_sample_version) {
                const auto version_it = image_sample_version->find(binding);
                if (version_it != image_sample_version->cend() && (version_it->second == cb_state.image_layout_change_count)) {
                    stale = false;
                }
            }
            if (stale) {
                out_req->emplace(binding_req_pair);
            }
        }
    }
}

void cvdescriptorset::DescriptorSet::UpdateValidationCache(const CMD_BUFFER_STATE &cb_state, const PIPELINE_STATE &pipeline,
                                                           const BindingReqMap &updated_bindings) {
    // For const cleanliness we have to find in the maps...
    auto &validated = cached_validation_[&cb_state];

    auto &image_sample_version = validated.image_samplers[&pipeline];
    auto &dynamic_buffers = validated.dynamic_buffers;
    auto &non_dynamic_buffers = validated.non_dynamic_buffers;
    for (const auto &binding_req_pair : updated_bindings) {
        auto binding = binding_req_pair.first;
        VkDescriptorSetLayoutBinding const *layout_binding = layout_->GetDescriptorSetLayoutBindingPtrFromBinding(binding);
        if (!layout_binding) {
            continue;
        }
        // Caching criteria differs per type.
        if (IsBufferDescriptor(layout_binding->descriptorType)) {
            if (IsDynamicDescriptor(layout_binding->descriptorType)) {
                dynamic_buffers.emplace(binding);
            } else {
                non_dynamic_buffers.emplace(binding);
            }
        } else {
            // Save the layout change version...
            image_sample_version[binding] = cb_state.image_layout_change_count;
        }
    }
}

cvdescriptorset::SamplerDescriptor::SamplerDescriptor(const ValidationStateTracker *dev_data, const VkSampler *immut)
    : immutable_(false) {
    updated = false;
    descriptor_class = PlainSampler;
    if (immut) {
        sampler_state_ = dev_data->GetConstCastShared<SAMPLER_STATE>(*immut);
        immutable_ = true;
        updated = true;
    }
}
// Validate given sampler. Currently this only checks to make sure it exists in the samplerMap
bool CoreChecks::ValidateSampler(const VkSampler sampler) const { return (GetSamplerState(sampler) != nullptr); }

bool CoreChecks::ValidateImageUpdate(VkImageView image_view, VkImageLayout image_layout, VkDescriptorType type,
                                     const char *func_name, std::string *error_code, std::string *error_msg) const {
    auto iv_state = GetImageViewState(image_view);
    assert(iv_state);

    // Note that when an imageview is created, we validated that memory is bound so no need to re-check here
    // Validate that imageLayout is compatible with aspect_mask and image format
    //  and validate that image usage bits are correct for given usage
    VkImageAspectFlags aspect_mask = iv_state->create_info.subresourceRange.aspectMask;
    VkImage image = iv_state->create_info.image;
    VkFormat format = VK_FORMAT_MAX_ENUM;
    VkImageUsageFlags usage = 0;
    auto image_node = GetImageState(image);
    assert(image_node);

    format = image_node->createInfo.format;
    usage = image_node->createInfo.usage;
    const auto stencil_usage_info = LvlFindInChain<VkImageStencilUsageCreateInfo>(image_node->createInfo.pNext);
    if (stencil_usage_info) {
        usage |= stencil_usage_info->stencilUsage;
    }

    // Validate that memory is bound to image
    if (ValidateMemoryIsBoundToImage(image_node, func_name, "UNASSIGNED-CoreValidation-BoundResourceFreedMemoryAccess")) {
        *error_code = "UNASSIGNED-CoreValidation-BoundResourceFreedMemoryAccess";
        *error_msg = "No memory bound to image.";
        return false;
    }

    // KHR_maintenance1 allows rendering into 2D or 2DArray views which slice a 3D image,
    // but not binding them to descriptor sets.
    if (image_node->createInfo.imageType == VK_IMAGE_TYPE_3D && (iv_state->create_info.viewType == VK_IMAGE_VIEW_TYPE_2D ||
                                                                 iv_state->create_info.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY)) {
        *error_code = "VUID-VkDescriptorImageInfo-imageView-00343";
        *error_msg = "ImageView must not be a 2D or 2DArray view of a 3D image";
        return false;
    }

    // TODO : The various image aspect and format checks here are based on general spec language in 11.5 Image Views section under
    // vkCreateImageView(). What's the best way to create unique id for these cases?
    *error_code = "UNASSIGNED-CoreValidation-DrawState-InvalidImageView";
    bool ds = FormatIsDepthOrStencil(format);
    switch (image_layout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            // Only Color bit must be set
            if ((aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) != VK_IMAGE_ASPECT_COLOR_BIT) {
                std::stringstream error_str;
                error_str
                    << "ImageView (" << report_data->FormatHandle(image_view)
                    << ") uses layout VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL but does not have VK_IMAGE_ASPECT_COLOR_BIT set.";
                *error_msg = error_str.str();
                return false;
            }
            // format must NOT be DS
            if (ds) {
                std::stringstream error_str;
                error_str << "ImageView (" << report_data->FormatHandle(image_view)
                          << ") uses layout VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL but the image format is "
                          << string_VkFormat(format) << " which is not a color format.";
                *error_msg = error_str.str();
                return false;
            }
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            // Depth or stencil bit must be set, but both must NOT be set
            if (aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) {
                if (aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) {
                    // both  must NOT be set
                    std::stringstream error_str;
                    error_str << "ImageView (" << report_data->FormatHandle(image_view)
                              << ") has both STENCIL and DEPTH aspects set";
                    *error_msg = error_str.str();
                    return false;
                }
            } else if (!(aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT)) {
                // Neither were set
                std::stringstream error_str;
                error_str << "ImageView (" << report_data->FormatHandle(image_view) << ") has layout "
                          << string_VkImageLayout(image_layout) << " but does not have STENCIL or DEPTH aspects set";
                *error_msg = error_str.str();
                return false;
            }
            // format must be DS
            if (!ds) {
                std::stringstream error_str;
                error_str << "ImageView (" << report_data->FormatHandle(image_view) << ") has layout "
                          << string_VkImageLayout(image_layout) << " but the image format is " << string_VkFormat(format)
                          << " which is not a depth/stencil format.";
                *error_msg = error_str.str();
                return false;
            }
            break;
        default:
            // For other layouts if the source is depth/stencil image, both aspect bits must not be set
            if (ds) {
                if (aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) {
                    if (aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) {
                        // both  must NOT be set
                        std::stringstream error_str;
                        error_str << "ImageView (" << report_data->FormatHandle(image_view) << ") has layout "
                                  << string_VkImageLayout(image_layout) << " and is using depth/stencil image of format "
                                  << string_VkFormat(format)
                                  << " but it has both STENCIL and DEPTH aspects set, which is illegal. When using a depth/stencil "
                                     "image in a descriptor set, please only set either VK_IMAGE_ASPECT_DEPTH_BIT or "
                                     "VK_IMAGE_ASPECT_STENCIL_BIT depending on whether it will be used for depth reads or stencil "
                                     "reads respectively.";
                        *error_code = "VUID-VkDescriptorImageInfo-imageView-01976";
                        *error_msg = error_str.str();
                        return false;
                    }
                }
            }
            break;
    }
    // Now validate that usage flags are correctly set for given type of update
    //  As we're switching per-type, if any type has specific layout requirements, check those here as well
    // TODO : The various image usage bit requirements are in general spec language for VkImageUsageFlags bit block in 11.3 Images
    // under vkCreateImage()
    const char *error_usage_bit = nullptr;
    switch (type) {
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            if (iv_state->samplerConversion != VK_NULL_HANDLE) {
                *error_code = "VUID-VkWriteDescriptorSet-descriptorType-01946";
                std::stringstream error_str;
                error_str << "ImageView (" << report_data->FormatHandle(image_view) << ")"
                          << "used as a VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE can't be created with VkSamplerYcbcrConversion";
                *error_msg = error_str.str();
                return false;
            }
            // drop through
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
            if (!(usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
                error_usage_bit = "VK_IMAGE_USAGE_SAMPLED_BIT";
                *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00337";
            }
            break;
        }
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
            if (!(usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
                error_usage_bit = "VK_IMAGE_USAGE_STORAGE_BIT";
                *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00339";
            } else if ((VK_IMAGE_LAYOUT_GENERAL != image_layout) || ((VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR != image_layout) &&
                                                                     (device_extensions.vk_khr_shared_presentable_image))) {
                *error_code = "VUID-VkWriteDescriptorSet-descriptorType-04152";
                std::stringstream error_str;
                error_str << "Descriptor update with descriptorType VK_DESCRIPTOR_TYPE_STORAGE_IMAGE"
                          << " is being updated with invalid imageLayout " << string_VkImageLayout(image_layout) << " for image "
                          << report_data->FormatHandle(image) << " in imageView " << report_data->FormatHandle(image_view)
                          << ". Allowed layouts are: VK_IMAGE_LAYOUT_GENERAL";
                if (device_extensions.vk_khr_shared_presentable_image) {
                    error_str << " or VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR";
                }
                *error_msg = error_str.str();
                return false;
            }
            break;
        }
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
            if (!(usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) {
                error_usage_bit = "VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT";
                *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00338";
            }
            break;
        }
        default:
            break;
    }
    if (error_usage_bit) {
        std::stringstream error_str;
        error_str << "ImageView (" << report_data->FormatHandle(image_view) << ") with usage mask " << std::hex << std::showbase
                  << usage << " being used for a descriptor update of type " << string_VkDescriptorType(type) << " does not have "
                  << error_usage_bit << " set.";
        *error_msg = error_str.str();
        return false;
    }

    // All the following types share the same image layouts
    // checkf or Storage Images above
    if ((type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) || (type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ||
        (type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)) {
        // Test that the layout is compatible with the descriptorType for the two sampled image types
        const static std::array<VkImageLayout, 3> valid_layouts = {
            {VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL}};

        struct ExtensionLayout {
            VkImageLayout layout;
            ExtEnabled DeviceExtensions::*extension;
        };
        const static std::array<ExtensionLayout, 5> extended_layouts{{
            //  Note double brace req'd for aggregate initialization
            {VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR, &DeviceExtensions::vk_khr_shared_presentable_image},
            {VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL, &DeviceExtensions::vk_khr_maintenance2},
            {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL, &DeviceExtensions::vk_khr_maintenance2},
            {VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL_KHR, &DeviceExtensions::vk_khr_synchronization_2},
            {VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR, &DeviceExtensions::vk_khr_synchronization_2},
        }};
        auto is_layout = [image_layout, this](const ExtensionLayout &ext_layout) {
            return device_extensions.*(ext_layout.extension) && (ext_layout.layout == image_layout);
        };

        bool valid_layout = (std::find(valid_layouts.cbegin(), valid_layouts.cend(), image_layout) != valid_layouts.cend()) ||
                            std::any_of(extended_layouts.cbegin(), extended_layouts.cend(), is_layout);

        if (!valid_layout) {
            // The following works as currently all 3 descriptor types share the same set of valid layouts
            switch (type) {
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                    *error_code = "VUID-VkWriteDescriptorSet-descriptorType-04149";
                    break;
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    *error_code = "VUID-VkWriteDescriptorSet-descriptorType-04150";
                    break;
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    *error_code = "VUID-VkWriteDescriptorSet-descriptorType-04151";
                    break;
                default:
                    break;
            }
            std::stringstream error_str;
            error_str << "Descriptor update with descriptorType " << string_VkDescriptorType(type)
                      << " is being updated with invalid imageLayout " << string_VkImageLayout(image_layout) << " for image "
                      << report_data->FormatHandle(image) << " in imageView " << report_data->FormatHandle(image_view)
                      << ". Allowed layouts are: VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, "
                      << "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL";
            for (auto &ext_layout : extended_layouts) {
                if (device_extensions.*(ext_layout.extension)) {
                    error_str << ", " << string_VkImageLayout(ext_layout.layout);
                }
            }
            *error_msg = error_str.str();
            return false;
        }
    }

    if ((type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) || (type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)) {
        const VkComponentMapping components = iv_state->create_info.components;
        if (IsIdentitySwizzle(components) == false) {
            *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00336";
            std::stringstream error_str;
            error_str << "ImageView (" << report_data->FormatHandle(image_view) << ") has a non-identiy swizzle component, "
                      << " r swizzle = " << string_VkComponentSwizzle(components.r) << ","
                      << " g swizzle = " << string_VkComponentSwizzle(components.g) << ","
                      << " b swizzle = " << string_VkComponentSwizzle(components.b) << ","
                      << " a swizzle = " << string_VkComponentSwizzle(components.a) << ".";
            *error_msg = error_str.str();
            return false;
        }
    }

    return true;
}

void cvdescriptorset::SamplerDescriptor::WriteUpdate(const ValidationStateTracker *dev_data, const VkWriteDescriptorSet *update,
                                                     const uint32_t index) {
    if (!immutable_) {
        sampler_state_ = dev_data->GetConstCastShared<SAMPLER_STATE>(update->pImageInfo[index].sampler);
    }
    updated = true;
}

void cvdescriptorset::SamplerDescriptor::CopyUpdate(const ValidationStateTracker *dev_data, const Descriptor *src) {
    if (!immutable_) {
        sampler_state_ = static_cast<const SamplerDescriptor *>(src)->sampler_state_;
    }
    updated = true;
}

void cvdescriptorset::SamplerDescriptor::UpdateDrawState(ValidationStateTracker *dev_data, CMD_BUFFER_STATE *cb_node) {
    if (!immutable_) {
        auto sampler_state = GetSamplerState();
        if (sampler_state) dev_data->AddCommandBufferBindingSampler(cb_node, sampler_state);
    }
}

cvdescriptorset::ImageSamplerDescriptor::ImageSamplerDescriptor(const ValidationStateTracker *dev_data, const VkSampler *immut)
    : immutable_(false), image_layout_(VK_IMAGE_LAYOUT_UNDEFINED) {
    updated = false;
    descriptor_class = ImageSampler;
    if (immut) {
        sampler_state_ = dev_data->GetConstCastShared<SAMPLER_STATE>(*immut);
        immutable_ = true;
    }
}

void cvdescriptorset::ImageSamplerDescriptor::WriteUpdate(const ValidationStateTracker *dev_data,
                                                          const VkWriteDescriptorSet *update, const uint32_t index) {
    updated = true;
    const auto &image_info = update->pImageInfo[index];
    if (!immutable_) {
        sampler_state_ = dev_data->GetConstCastShared<SAMPLER_STATE>(image_info.sampler);
    }
    image_layout_ = image_info.imageLayout;
    image_view_state_ = dev_data->GetConstCastShared<IMAGE_VIEW_STATE>(image_info.imageView);
}

void cvdescriptorset::ImageSamplerDescriptor::CopyUpdate(const ValidationStateTracker *dev_data, const Descriptor *src) {
    if (!immutable_) {
        sampler_state_ = static_cast<const ImageSamplerDescriptor *>(src)->sampler_state_;
    }
    updated = true;
    image_layout_ = static_cast<const ImageSamplerDescriptor *>(src)->image_layout_;
    image_view_state_ = static_cast<const ImageSamplerDescriptor *>(src)->image_view_state_;
}

void cvdescriptorset::ImageSamplerDescriptor::UpdateDrawState(ValidationStateTracker *dev_data, CMD_BUFFER_STATE *cb_node) {
    // First add binding for any non-immutable sampler
    if (!immutable_) {
        auto sampler_state = GetSamplerState();
        if (sampler_state) dev_data->AddCommandBufferBindingSampler(cb_node, sampler_state);
    }
    // Add binding for image
    auto iv_state = GetImageViewState();
    if (iv_state) {
        dev_data->AddCommandBufferBindingImageView(cb_node, iv_state);
        dev_data->CallSetImageViewInitialLayoutCallback(cb_node, *iv_state, image_layout_);
    }
}

cvdescriptorset::ImageDescriptor::ImageDescriptor(const VkDescriptorType type) : image_layout_(VK_IMAGE_LAYOUT_UNDEFINED) {
    updated = false;
    descriptor_class = Image;
}

void cvdescriptorset::ImageDescriptor::WriteUpdate(const ValidationStateTracker *dev_data, const VkWriteDescriptorSet *update,
                                                   const uint32_t index) {
    updated = true;
    const auto &image_info = update->pImageInfo[index];
    image_layout_ = image_info.imageLayout;
    image_view_state_ = dev_data->GetConstCastShared<IMAGE_VIEW_STATE>(image_info.imageView);
}

void cvdescriptorset::ImageDescriptor::CopyUpdate(const ValidationStateTracker *dev_data, const Descriptor *src) {
    updated = true;
    image_layout_ = static_cast<const ImageDescriptor *>(src)->image_layout_;
    image_view_state_ = static_cast<const ImageDescriptor *>(src)->image_view_state_;
}

void cvdescriptorset::ImageDescriptor::UpdateDrawState(ValidationStateTracker *dev_data, CMD_BUFFER_STATE *cb_node) {
    // Add binding for image
    auto iv_state = GetImageViewState();
    if (iv_state) {
        dev_data->AddCommandBufferBindingImageView(cb_node, iv_state);
        dev_data->CallSetImageViewInitialLayoutCallback(cb_node, *iv_state, image_layout_);
    }
}

cvdescriptorset::BufferDescriptor::BufferDescriptor(const VkDescriptorType type) : offset_(0), range_(0) {
    updated = false;
    descriptor_class = GeneralBuffer;
}
void cvdescriptorset::BufferDescriptor::WriteUpdate(const ValidationStateTracker *dev_data, const VkWriteDescriptorSet *update,
                                                    const uint32_t index) {
    updated = true;
    const auto &buffer_info = update->pBufferInfo[index];
    offset_ = buffer_info.offset;
    range_ = buffer_info.range;
    buffer_state_ = dev_data->GetConstCastShared<BUFFER_STATE>(buffer_info.buffer);
}

void cvdescriptorset::BufferDescriptor::CopyUpdate(const ValidationStateTracker *dev_data, const Descriptor *src) {
    updated = true;
    const auto buff_desc = static_cast<const BufferDescriptor *>(src);
    offset_ = buff_desc->offset_;
    range_ = buff_desc->range_;
    buffer_state_ = buff_desc->buffer_state_;
}

void cvdescriptorset::BufferDescriptor::UpdateDrawState(ValidationStateTracker *dev_data, CMD_BUFFER_STATE *cb_node) {
    auto buffer_node = GetBufferState();
    if (buffer_node) dev_data->AddCommandBufferBindingBuffer(cb_node, buffer_node);
}

cvdescriptorset::TexelDescriptor::TexelDescriptor(const VkDescriptorType type) {
    updated = false;
    descriptor_class = TexelBuffer;
}

void cvdescriptorset::TexelDescriptor::WriteUpdate(const ValidationStateTracker *dev_data, const VkWriteDescriptorSet *update,
                                                   const uint32_t index) {
    updated = true;
    buffer_view_state_ = dev_data->GetConstCastShared<BUFFER_VIEW_STATE>(update->pTexelBufferView[index]);
}

void cvdescriptorset::TexelDescriptor::CopyUpdate(const ValidationStateTracker *dev_data, const Descriptor *src) {
    updated = true;
    buffer_view_state_ = static_cast<const TexelDescriptor *>(src)->buffer_view_state_;
}

void cvdescriptorset::TexelDescriptor::UpdateDrawState(ValidationStateTracker *dev_data, CMD_BUFFER_STATE *cb_node) {
    auto bv_state = GetBufferViewState();
    if (bv_state) {
        dev_data->AddCommandBufferBindingBufferView(cb_node, bv_state);
    }
}

cvdescriptorset::AccelerationStructureDescriptor::AccelerationStructureDescriptor(const VkDescriptorType type)
    : acc_(VK_NULL_HANDLE), acc_nv_(VK_NULL_HANDLE) {
    updated = false;
    is_khr_ = false;
    descriptor_class = AccelerationStructure;
}
void cvdescriptorset::AccelerationStructureDescriptor::WriteUpdate(const ValidationStateTracker *dev_data,
                                                                   const VkWriteDescriptorSet *update, const uint32_t index) {
    const auto *acc_info = LvlFindInChain<VkWriteDescriptorSetAccelerationStructureKHR>(update->pNext);
    const auto *acc_info_nv = LvlFindInChain<VkWriteDescriptorSetAccelerationStructureNV>(update->pNext);
    assert(acc_info || acc_info_nv);
    is_khr_ = (acc_info != NULL);
    updated = true;
    if (is_khr_) {
        acc_ = acc_info->pAccelerationStructures[index];
        acc_state_ = dev_data->GetConstCastShared<ACCELERATION_STRUCTURE_STATE_KHR>(acc_);
    } else {
        acc_nv_ = acc_info_nv->pAccelerationStructures[index];
        acc_state_nv_ = dev_data->GetConstCastShared<ACCELERATION_STRUCTURE_STATE>(acc_nv_);
    }
}

void cvdescriptorset::AccelerationStructureDescriptor::CopyUpdate(const ValidationStateTracker *dev_data, const Descriptor *src) {
    auto acc_desc = static_cast<const AccelerationStructureDescriptor *>(src);
    updated = true;
    if (is_khr_) {
        acc_ = acc_desc->acc_;
        acc_state_ = dev_data->GetConstCastShared<ACCELERATION_STRUCTURE_STATE_KHR>(acc_);
    } else {
        acc_nv_ = acc_desc->acc_nv_;
        acc_state_nv_ = dev_data->GetConstCastShared<ACCELERATION_STRUCTURE_STATE>(acc_nv_);
    }
}

void cvdescriptorset::AccelerationStructureDescriptor::UpdateDrawState(ValidationStateTracker *dev_data,
                                                                       CMD_BUFFER_STATE *cb_node) {
    if (is_khr_) {
        auto acc_node = GetAccelerationStructureStateKHR();
        if (acc_node) dev_data->AddCommandBufferBindingAccelerationStructure(cb_node, acc_node);
    } else {
        auto acc_node = GetAccelerationStructureStateNV();
        if (acc_node) dev_data->AddCommandBufferBindingAccelerationStructure(cb_node, acc_node);
    }
}

// This is a helper function that iterates over a set of Write and Copy updates, pulls the DescriptorSet* for updated
//  sets, and then calls their respective Validate[Write|Copy]Update functions.
// If the update hits an issue for which the callback returns "true", meaning that the call down the chain should
//  be skipped, then true is returned.
// If there is no issue with the update, then false is returned.
bool CoreChecks::ValidateUpdateDescriptorSets(uint32_t write_count, const VkWriteDescriptorSet *p_wds, uint32_t copy_count,
                                              const VkCopyDescriptorSet *p_cds, const char *func_name) const {
    bool skip = false;
    // Validate Write updates
    for (uint32_t i = 0; i < write_count; i++) {
        auto dest_set = p_wds[i].dstSet;
        auto set_node = GetSetNode(dest_set);
        if (!set_node) {
            skip |= LogError(dest_set, kVUID_Core_DrawState_InvalidDescriptorSet,
                             "Cannot call %s on %s that has not been allocated in pDescriptorWrites[%u].", func_name,
                             report_data->FormatHandle(dest_set).c_str(), i);
        } else {
            std::string error_code;
            std::string error_str;
            if (!ValidateWriteUpdate(set_node, &p_wds[i], func_name, &error_code, &error_str)) {
                skip |=
                    LogError(dest_set, error_code, "%s pDescriptorWrites[%u] failed write update validation for %s with error: %s.",
                             func_name, i, report_data->FormatHandle(dest_set).c_str(), error_str.c_str());
            }
        }
        if (p_wds[i].pNext) {
            const auto *pnext_struct = LvlFindInChain<VkWriteDescriptorSetAccelerationStructureKHR>(p_wds[i].pNext);
            if (pnext_struct) {
                for (uint32_t j = 0; j < pnext_struct->accelerationStructureCount; ++j) {
                    const ACCELERATION_STRUCTURE_STATE_KHR *as_state =
                        GetAccelerationStructureStateKHR(pnext_struct->pAccelerationStructures[j]);
                    if (as_state && (as_state->create_infoKHR.sType == VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR &&
                                     (as_state->create_infoKHR.type != VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR &&
                                      as_state->create_infoKHR.type != VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR))) {
                        skip |=
                            LogError(dest_set, "VUID-VkWriteDescriptorSetAccelerationStructureKHR-pAccelerationStructures-03579",
                                     "%s: For pDescriptorWrites[%u] acceleration structure in pAccelerationStructures[%u] must "
                                     "have been created with "
                                     "VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR or VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR.",
                                     func_name, i, j);
                    }
                }
            }
            const auto *pnext_struct_nv = LvlFindInChain<VkWriteDescriptorSetAccelerationStructureNV>(p_wds[i].pNext);
            if (pnext_struct_nv) {
                for (uint32_t j = 0; j < pnext_struct_nv->accelerationStructureCount; ++j) {
                    const ACCELERATION_STRUCTURE_STATE *as_state =
                        GetAccelerationStructureStateNV(pnext_struct_nv->pAccelerationStructures[j]);
                    if (as_state && (as_state->create_infoNV.sType == VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_NV &&
                                     as_state->create_infoNV.info.type != VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_NV)) {
                        skip |= LogError(dest_set, "VUID-VkWriteDescriptorSetAccelerationStructureNV-pAccelerationStructures-03748",
                                         "%s: For pDescriptorWrites[%u] acceleration structure in pAccelerationStructures[%u] must "
                                         "have been created with"
                                         " VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_NV.",
                                         func_name, i, j);
                    }
                }
            }
        }
    }
    // Now validate copy updates
    for (uint32_t i = 0; i < copy_count; ++i) {
        auto dst_set = p_cds[i].dstSet;
        auto src_set = p_cds[i].srcSet;
        auto src_node = GetSetNode(src_set);
        auto dst_node = GetSetNode(dst_set);
        // Object_tracker verifies that src & dest descriptor set are valid
        assert(src_node);
        assert(dst_node);
        std::string error_code;
        std::string error_str;
        if (!ValidateCopyUpdate(&p_cds[i], dst_node, src_node, func_name, &error_code, &error_str)) {
            LogObjectList objlist(dst_set);
            objlist.add(src_set);
            skip |= LogError(objlist, error_code, "%s pDescriptorCopies[%u] failed copy update from %s to %s with error: %s.",
                             func_name, i, report_data->FormatHandle(src_set).c_str(), report_data->FormatHandle(dst_set).c_str(),
                             error_str.c_str());
        }
    }
    return skip;
}
// This is a helper function that iterates over a set of Write and Copy updates, pulls the DescriptorSet* for updated
//  sets, and then calls their respective Perform[Write|Copy]Update functions.
// Prerequisite : ValidateUpdateDescriptorSets() should be called and return "false" prior to calling PerformUpdateDescriptorSets()
//  with the same set of updates.
// This is split from the validate code to allow validation prior to calling down the chain, and then update after
//  calling down the chain.
void cvdescriptorset::PerformUpdateDescriptorSets(ValidationStateTracker *dev_data, uint32_t write_count,
                                                  const VkWriteDescriptorSet *p_wds, uint32_t copy_count,
                                                  const VkCopyDescriptorSet *p_cds) {
    // Write updates first
    uint32_t i = 0;
    for (i = 0; i < write_count; ++i) {
        auto dest_set = p_wds[i].dstSet;
        auto set_node = dev_data->GetSetNode(dest_set);
        if (set_node) {
            set_node->PerformWriteUpdate(dev_data, &p_wds[i]);
        }
    }
    // Now copy updates
    for (i = 0; i < copy_count; ++i) {
        auto dst_set = p_cds[i].dstSet;
        auto src_set = p_cds[i].srcSet;
        auto src_node = dev_data->GetSetNode(src_set);
        auto dst_node = dev_data->GetSetNode(dst_set);
        if (src_node && dst_node) {
            dst_node->PerformCopyUpdate(dev_data, &p_cds[i], src_node);
        }
    }
}

cvdescriptorset::DecodedTemplateUpdate::DecodedTemplateUpdate(const ValidationStateTracker *device_data,
                                                              VkDescriptorSet descriptorSet, const TEMPLATE_STATE *template_state,
                                                              const void *pData, VkDescriptorSetLayout push_layout) {
    auto const &create_info = template_state->create_info;
    inline_infos.resize(create_info.descriptorUpdateEntryCount);  // Make sure we have one if we need it
    inline_infos_khr.resize(create_info.descriptorUpdateEntryCount);
    inline_infos_nv.resize(create_info.descriptorUpdateEntryCount);
    desc_writes.reserve(create_info.descriptorUpdateEntryCount);  // emplaced, so reserved without initialization
    VkDescriptorSetLayout effective_dsl = create_info.templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET
                                              ? create_info.descriptorSetLayout
                                              : push_layout;
    auto layout_obj = device_data->GetDescriptorSetLayoutShared(effective_dsl);

    // Create a WriteDescriptorSet struct for each template update entry
    for (uint32_t i = 0; i < create_info.descriptorUpdateEntryCount; i++) {
        auto binding_count = layout_obj->GetDescriptorCountFromBinding(create_info.pDescriptorUpdateEntries[i].dstBinding);
        auto binding_being_updated = create_info.pDescriptorUpdateEntries[i].dstBinding;
        auto dst_array_element = create_info.pDescriptorUpdateEntries[i].dstArrayElement;

        desc_writes.reserve(desc_writes.size() + create_info.pDescriptorUpdateEntries[i].descriptorCount);
        for (uint32_t j = 0; j < create_info.pDescriptorUpdateEntries[i].descriptorCount; j++) {
            desc_writes.emplace_back();
            auto &write_entry = desc_writes.back();

            size_t offset = create_info.pDescriptorUpdateEntries[i].offset + j * create_info.pDescriptorUpdateEntries[i].stride;
            char *update_entry = (char *)(pData) + offset;

            if (dst_array_element >= binding_count) {
                dst_array_element = 0;
                binding_being_updated = layout_obj->GetNextValidBinding(binding_being_updated);
            }

            write_entry.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_entry.pNext = NULL;
            write_entry.dstSet = descriptorSet;
            write_entry.dstBinding = binding_being_updated;
            write_entry.dstArrayElement = dst_array_element;
            write_entry.descriptorCount = 1;
            write_entry.descriptorType = create_info.pDescriptorUpdateEntries[i].descriptorType;

            switch (create_info.pDescriptorUpdateEntries[i].descriptorType) {
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    write_entry.pImageInfo = reinterpret_cast<VkDescriptorImageInfo *>(update_entry);
                    break;

                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                    write_entry.pBufferInfo = reinterpret_cast<VkDescriptorBufferInfo *>(update_entry);
                    break;

                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                    write_entry.pTexelBufferView = reinterpret_cast<VkBufferView *>(update_entry);
                    break;
                case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT: {
                    VkWriteDescriptorSetInlineUniformBlockEXT *inline_info = &inline_infos[i];
                    inline_info->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK_EXT;
                    inline_info->pNext = nullptr;
                    inline_info->dataSize = create_info.pDescriptorUpdateEntries[i].descriptorCount;
                    inline_info->pData = update_entry;
                    write_entry.pNext = inline_info;
                    // descriptorCount must match the dataSize member of the VkWriteDescriptorSetInlineUniformBlockEXT structure
                    write_entry.descriptorCount = inline_info->dataSize;
                    // skip the rest of the array, they just represent bytes in the update
                    j = create_info.pDescriptorUpdateEntries[i].descriptorCount;
                    break;
                }
                case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
                    VkWriteDescriptorSetAccelerationStructureKHR *inline_info_khr = &inline_infos_khr[i];
                    inline_info_khr->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
                    inline_info_khr->pNext = nullptr;
                    inline_info_khr->accelerationStructureCount = create_info.pDescriptorUpdateEntries[i].descriptorCount;
                    inline_info_khr->pAccelerationStructures = reinterpret_cast<VkAccelerationStructureKHR *>(update_entry);
                    write_entry.pNext = inline_info_khr;
                    break;
                }
                case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV: {
                    VkWriteDescriptorSetAccelerationStructureNV *inline_info_nv = &inline_infos_nv[i];
                    inline_info_nv->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_NV;
                    inline_info_nv->pNext = nullptr;
                    inline_info_nv->accelerationStructureCount = create_info.pDescriptorUpdateEntries[i].descriptorCount;
                    inline_info_nv->pAccelerationStructures = reinterpret_cast<VkAccelerationStructureNV *>(update_entry);
                    write_entry.pNext = inline_info_nv;
                    break;
                }
                default:
                    assert(0);
                    break;
            }
            dst_array_element++;
        }
    }
}
// These helper functions carry out the validate and record descriptor updates peformed via update templates. They decode
// the templatized data and leverage the non-template UpdateDescriptor helper functions.
bool CoreChecks::ValidateUpdateDescriptorSetsWithTemplateKHR(VkDescriptorSet descriptorSet, const TEMPLATE_STATE *template_state,
                                                             const void *pData) const {
    // Translate the templated update into a normal update for validation...
    cvdescriptorset::DecodedTemplateUpdate decoded_update(this, descriptorSet, template_state, pData);
    return ValidateUpdateDescriptorSets(static_cast<uint32_t>(decoded_update.desc_writes.size()), decoded_update.desc_writes.data(),
                                        0, NULL, "vkUpdateDescriptorSetWithTemplate()");
}

std::string cvdescriptorset::DescriptorSet::StringifySetAndLayout() const {
    std::string out;
    auto layout_handle = layout_->GetDescriptorSetLayout();
    if (IsPushDescriptor()) {
        std::ostringstream str;
        str << "Push Descriptors defined with " << state_data_->report_data->FormatHandle(layout_handle);
        out = str.str();
    } else {
        std::ostringstream str;
        str << state_data_->report_data->FormatHandle(set_) << " allocated with "
            << state_data_->report_data->FormatHandle(layout_handle);
        out = str.str();
    }
    return out;
};

// Loop through the write updates to validate for a push descriptor set, ignoring dstSet
bool CoreChecks::ValidatePushDescriptorsUpdate(const DescriptorSet *push_set, uint32_t write_count,
                                               const VkWriteDescriptorSet *p_wds, const char *func_name) const {
    assert(push_set->IsPushDescriptor());
    bool skip = false;
    for (uint32_t i = 0; i < write_count; i++) {
        std::string error_code;
        std::string error_str;
        if (!ValidateWriteUpdate(push_set, &p_wds[i], func_name, &error_code, &error_str)) {
            skip |= LogError(push_set->GetDescriptorSetLayout(), error_code,
                             "%s VkWriteDescriptorSet[%u] failed update validation: %s.", func_name, i, error_str.c_str());
        }
    }
    return skip;
}

// For the given buffer, verify that its creation parameters are appropriate for the given type
//  If there's an error, update the error_msg string with details and return false, else return true
bool cvdescriptorset::ValidateBufferUsage(debug_report_data *report_data, BUFFER_STATE const *buffer_node, VkDescriptorType type,
                                          std::string *error_code, std::string *error_msg) {
    // Verify that usage bits set correctly for given type
    auto usage = buffer_node->createInfo.usage;
    const char *error_usage_bit = nullptr;
    switch (type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            if (!(usage & VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)) {
                *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00334";
                error_usage_bit = "VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT";
            }
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            if (!(usage & VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) {
                *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00335";
                error_usage_bit = "VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT";
            }
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            if (!(usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
                *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00330";
                error_usage_bit = "VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT";
            }
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            if (!(usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
                *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00331";
                error_usage_bit = "VK_BUFFER_USAGE_STORAGE_BUFFER_BIT";
            }
            break;
        default:
            break;
    }
    if (error_usage_bit) {
        std::stringstream error_str;
        error_str << "Buffer (" << report_data->FormatHandle(buffer_node->buffer) << ") with usage mask " << std::hex
                  << std::showbase << usage << " being used for a descriptor update of type " << string_VkDescriptorType(type)
                  << " does not have " << error_usage_bit << " set.";
        *error_msg = error_str.str();
        return false;
    }
    return true;
}
// For buffer descriptor updates, verify the buffer usage and VkDescriptorBufferInfo struct which includes:
//  1. buffer is valid
//  2. buffer was created with correct usage flags
//  3. offset is less than buffer size
//  4. range is either VK_WHOLE_SIZE or falls in (0, (buffer size - offset)]
//  5. range and offset are within the device's limits
// If there's an error, update the error_msg string with details and return false, else return true
bool CoreChecks::ValidateBufferUpdate(VkDescriptorBufferInfo const *buffer_info, VkDescriptorType type, const char *func_name,
                                      std::string *error_code, std::string *error_msg) const {
    // First make sure that buffer is valid
    auto buffer_node = GetBufferState(buffer_info->buffer);
    // Any invalid buffer should already be caught by object_tracker
    assert(buffer_node);
    if (ValidateMemoryIsBoundToBuffer(buffer_node, func_name, "VUID-VkWriteDescriptorSet-descriptorType-00329")) {
        *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00329";
        *error_msg = "No memory bound to buffer.";
        return false;
    }
    // Verify usage bits
    if (!cvdescriptorset::ValidateBufferUsage(report_data, buffer_node, type, error_code, error_msg)) {
        // error_msg will have been updated by ValidateBufferUsage()
        return false;
    }
    // offset must be less than buffer size
    if (buffer_info->offset >= buffer_node->createInfo.size) {
        *error_code = "VUID-VkDescriptorBufferInfo-offset-00340";
        std::stringstream error_str;
        error_str << "VkDescriptorBufferInfo offset of " << buffer_info->offset << " is greater than or equal to buffer "
                  << report_data->FormatHandle(buffer_node->buffer) << " size of " << buffer_node->createInfo.size;
        *error_msg = error_str.str();
        return false;
    }
    if (buffer_info->range != VK_WHOLE_SIZE) {
        // Range must be VK_WHOLE_SIZE or > 0
        if (!buffer_info->range) {
            *error_code = "VUID-VkDescriptorBufferInfo-range-00341";
            std::stringstream error_str;
            error_str << "For buffer " << report_data->FormatHandle(buffer_node->buffer)
                      << " VkDescriptorBufferInfo range is not VK_WHOLE_SIZE and is zero, which is not allowed.";
            *error_msg = error_str.str();
            return false;
        }
        // Range must be VK_WHOLE_SIZE or <= (buffer size - offset)
        if (buffer_info->range > (buffer_node->createInfo.size - buffer_info->offset)) {
            *error_code = "VUID-VkDescriptorBufferInfo-range-00342";
            std::stringstream error_str;
            error_str << "For buffer " << report_data->FormatHandle(buffer_node->buffer) << " VkDescriptorBufferInfo range is "
                      << buffer_info->range << " which is greater than buffer size (" << buffer_node->createInfo.size
                      << ") minus requested offset of " << buffer_info->offset;
            *error_msg = error_str.str();
            return false;
        }
    }
    // Check buffer update sizes against device limits
    const auto &limits = phys_dev_props.limits;
    if (VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER == type || VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC == type) {
        auto max_ub_range = limits.maxUniformBufferRange;
        if (buffer_info->range != VK_WHOLE_SIZE && buffer_info->range > max_ub_range) {
            *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00332";
            std::stringstream error_str;
            error_str << "For buffer " << report_data->FormatHandle(buffer_node->buffer) << " VkDescriptorBufferInfo range is "
                      << buffer_info->range << " which is greater than this device's maxUniformBufferRange (" << max_ub_range
                      << ")";
            *error_msg = error_str.str();
            return false;
        } else if (buffer_info->range == VK_WHOLE_SIZE && (buffer_node->createInfo.size - buffer_info->offset) > max_ub_range) {
            *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00332";
            std::stringstream error_str;
            error_str << "For buffer " << report_data->FormatHandle(buffer_node->buffer)
                      << " VkDescriptorBufferInfo range is VK_WHOLE_SIZE but effective range "
                      << "(" << (buffer_node->createInfo.size - buffer_info->offset) << ") is greater than this device's "
                      << "maxUniformBufferRange (" << max_ub_range << ")";
            *error_msg = error_str.str();
            return false;
        }
    } else if (VK_DESCRIPTOR_TYPE_STORAGE_BUFFER == type || VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC == type) {
        auto max_sb_range = limits.maxStorageBufferRange;
        if (buffer_info->range != VK_WHOLE_SIZE && buffer_info->range > max_sb_range) {
            *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00333";
            std::stringstream error_str;
            error_str << "For buffer " << report_data->FormatHandle(buffer_node->buffer) << " VkDescriptorBufferInfo range is "
                      << buffer_info->range << " which is greater than this device's maxStorageBufferRange (" << max_sb_range
                      << ")";
            *error_msg = error_str.str();
            return false;
        } else if (buffer_info->range == VK_WHOLE_SIZE && (buffer_node->createInfo.size - buffer_info->offset) > max_sb_range) {
            *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00333";
            std::stringstream error_str;
            error_str << "For buffer " << report_data->FormatHandle(buffer_node->buffer)
                      << " VkDescriptorBufferInfo range is VK_WHOLE_SIZE but effective range "
                      << "(" << (buffer_node->createInfo.size - buffer_info->offset) << ") is greater than this device's "
                      << "maxStorageBufferRange (" << max_sb_range << ")";
            *error_msg = error_str.str();
            return false;
        }
    }
    return true;
}
template <typename T>
bool CoreChecks::ValidateAccelerationStructureUpdate(T acc_node, const char *func_name, std::string *error_code,
                                                     std::string *error_msg) const {
    // Any invalid acc struct should already be caught by object_tracker
    assert(acc_node);
    if (ValidateMemoryIsBoundToAccelerationStructure(acc_node, func_name, kVUIDUndefined)) {
        *error_code = kVUIDUndefined;
        *error_msg = "No memory bound to acceleration structure.";
        return false;
    }
    return true;
}

// Verify that the contents of the update are ok, but don't perform actual update
bool CoreChecks::VerifyCopyUpdateContents(const VkCopyDescriptorSet *update, const DescriptorSet *src_set,
                                          VkDescriptorType src_type, uint32_t src_index, const DescriptorSet *dst_set,
                                          VkDescriptorType dst_type, uint32_t dst_index, const char *func_name,
                                          std::string *error_code, std::string *error_msg) const {
    // Note : Repurposing some Write update error codes here as specific details aren't called out for copy updates like they are
    // for write updates
    using DescriptorClass = cvdescriptorset::DescriptorClass;
    using BufferDescriptor = cvdescriptorset::BufferDescriptor;
    using ImageDescriptor = cvdescriptorset::ImageDescriptor;
    using ImageSamplerDescriptor = cvdescriptorset::ImageSamplerDescriptor;
    using SamplerDescriptor = cvdescriptorset::SamplerDescriptor;
    using TexelDescriptor = cvdescriptorset::TexelDescriptor;

    auto device_data = this;

    if (dst_type == VK_DESCRIPTOR_TYPE_SAMPLER) {
        for (uint32_t di = 0; di < update->descriptorCount; ++di) {
            const auto dst_desc = dst_set->GetDescriptorFromGlobalIndex(dst_index + di);
            if (!dst_desc->updated) continue;
            if (dst_desc->IsImmutableSampler()) {
                *error_code = "VUID-VkCopyDescriptorSet-dstBinding-02753";
                std::stringstream error_str;
                error_str << "Attempted copy update to an immutable sampler descriptor.";
                *error_msg = error_str.str();
                return false;
            }
        }
    }

    switch (src_set->GetDescriptorFromGlobalIndex(src_index)->descriptor_class) {
        case DescriptorClass::PlainSampler: {
            for (uint32_t di = 0; di < update->descriptorCount; ++di) {
                const auto src_desc = src_set->GetDescriptorFromGlobalIndex(src_index + di);
                if (!src_desc->updated) continue;
                if (!src_desc->IsImmutableSampler()) {
                    auto update_sampler = static_cast<const SamplerDescriptor *>(src_desc)->GetSampler();
                    if (!ValidateSampler(update_sampler)) {
                        *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00325";
                        std::stringstream error_str;
                        error_str << "Attempted copy update to sampler descriptor with invalid sampler: "
                                  << report_data->FormatHandle(update_sampler) << ".";
                        *error_msg = error_str.str();
                        return false;
                    }
                } else {
                    // TODO : Warn here
                }
            }
            break;
        }
        case DescriptorClass::ImageSampler: {
            for (uint32_t di = 0; di < update->descriptorCount; ++di) {
                const auto src_desc = src_set->GetDescriptorFromGlobalIndex(src_index + di);
                if (!src_desc->updated) continue;
                auto img_samp_desc = static_cast<const ImageSamplerDescriptor *>(src_desc);
                // First validate sampler
                if (!img_samp_desc->IsImmutableSampler()) {
                    auto update_sampler = img_samp_desc->GetSampler();
                    if (!ValidateSampler(update_sampler)) {
                        *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00325";
                        std::stringstream error_str;
                        error_str << "Attempted copy update to sampler descriptor with invalid sampler: "
                                  << report_data->FormatHandle(update_sampler) << ".";
                        *error_msg = error_str.str();
                        return false;
                    }
                } else {
                    // TODO : Warn here
                }
                // Validate image
                auto image_view = img_samp_desc->GetImageView();
                auto image_layout = img_samp_desc->GetImageLayout();
                if (image_view) {
                    if (!ValidateImageUpdate(image_view, image_layout, src_type, func_name, error_code, error_msg)) {
                        std::stringstream error_str;
                        error_str << "Attempted copy update to combined image sampler descriptor failed due to: "
                                  << error_msg->c_str();
                        *error_msg = error_str.str();
                        return false;
                    }
                }
            }
            break;
        }
        case DescriptorClass::Image: {
            for (uint32_t di = 0; di < update->descriptorCount; ++di) {
                const auto src_desc = src_set->GetDescriptorFromGlobalIndex(src_index + di);
                if (!src_desc->updated) continue;
                auto img_desc = static_cast<const ImageDescriptor *>(src_desc);
                auto image_view = img_desc->GetImageView();
                auto image_layout = img_desc->GetImageLayout();
                if (image_view) {
                    if (!ValidateImageUpdate(image_view, image_layout, src_type, func_name, error_code, error_msg)) {
                        std::stringstream error_str;
                        error_str << "Attempted copy update to image descriptor failed due to: " << error_msg->c_str();
                        *error_msg = error_str.str();
                        return false;
                    }
                }
            }
            break;
        }
        case DescriptorClass::TexelBuffer: {
            for (uint32_t di = 0; di < update->descriptorCount; ++di) {
                const auto src_desc = src_set->GetDescriptorFromGlobalIndex(src_index + di);
                if (!src_desc->updated) continue;
                auto buffer_view = static_cast<const TexelDescriptor *>(src_desc)->GetBufferView();
                if (buffer_view) {
                    auto bv_state = device_data->GetBufferViewState(buffer_view);
                    if (!bv_state) {
                        *error_code = "VUID-VkWriteDescriptorSet-descriptorType-02994";
                        std::stringstream error_str;
                        error_str << "Attempted copy update to texel buffer descriptor with invalid buffer view: "
                                  << report_data->FormatHandle(buffer_view);
                        *error_msg = error_str.str();
                        return false;
                    }
                    auto buffer = bv_state->create_info.buffer;
                    if (!cvdescriptorset::ValidateBufferUsage(report_data, GetBufferState(buffer), src_type, error_code,
                                                              error_msg)) {
                        std::stringstream error_str;
                        error_str << "Attempted copy update to texel buffer descriptor failed due to: " << error_msg->c_str();
                        *error_msg = error_str.str();
                        return false;
                    }
                }
            }
            break;
        }
        case DescriptorClass::GeneralBuffer: {
            for (uint32_t di = 0; di < update->descriptorCount; ++di) {
                const auto src_desc = src_set->GetDescriptorFromGlobalIndex(src_index + di);
                if (!src_desc->updated) continue;
                auto buffer = static_cast<const BufferDescriptor *>(src_desc)->GetBuffer();
                if (buffer) {
                    if (!cvdescriptorset::ValidateBufferUsage(report_data, GetBufferState(buffer), src_type, error_code,
                                                              error_msg)) {
                        std::stringstream error_str;
                        error_str << "Attempted copy update to buffer descriptor failed due to: " << error_msg->c_str();
                        *error_msg = error_str.str();
                        return false;
                    }
                }
            }
            break;
        }
        case DescriptorClass::InlineUniform:
        case DescriptorClass::AccelerationStructure:
            break;
        default:
            assert(0);  // We've already verified update type so should never get here
            break;
    }
    // All checks passed so update contents are good
    return true;
}
// Verify that the state at allocate time is correct, but don't actually allocate the sets yet
bool CoreChecks::ValidateAllocateDescriptorSets(const VkDescriptorSetAllocateInfo *p_alloc_info,
                                                const cvdescriptorset::AllocateDescriptorSetsData *ds_data) const {
    bool skip = false;
    auto pool_state = GetDescriptorPoolState(p_alloc_info->descriptorPool);

    for (uint32_t i = 0; i < p_alloc_info->descriptorSetCount; i++) {
        auto layout = GetDescriptorSetLayoutShared(p_alloc_info->pSetLayouts[i]);
        if (layout) {  // nullptr layout indicates no valid layout handle for this device, validated/logged in object_tracker
            if (layout->IsPushDescriptor()) {
                skip |= LogError(p_alloc_info->pSetLayouts[i], "VUID-VkDescriptorSetAllocateInfo-pSetLayouts-00308",
                                 "%s specified at pSetLayouts[%" PRIu32
                                 "] in vkAllocateDescriptorSets() was created with invalid flag %s set.",
                                 report_data->FormatHandle(p_alloc_info->pSetLayouts[i]).c_str(), i,
                                 "VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR");
            }
            if (layout->GetCreateFlags() & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT &&
                !(pool_state->createInfo.flags & VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT)) {
                skip |= LogError(
                    device, "VUID-VkDescriptorSetAllocateInfo-pSetLayouts-03044",
                    "vkAllocateDescriptorSets(): Descriptor set layout create flags and pool create flags mismatch for index (%d)",
                    i);
            }
        }
    }
    if (!device_extensions.vk_khr_maintenance1) {
        // Track number of descriptorSets allowable in this pool
        if (pool_state->availableSets < p_alloc_info->descriptorSetCount) {
            skip |= LogError(pool_state->pool, "VUID-VkDescriptorSetAllocateInfo-descriptorSetCount-00306",
                             "vkAllocateDescriptorSets(): Unable to allocate %u descriptorSets from %s"
                             ". This pool only has %d descriptorSets remaining.",
                             p_alloc_info->descriptorSetCount, report_data->FormatHandle(pool_state->pool).c_str(),
                             pool_state->availableSets);
        }
        // Determine whether descriptor counts are satisfiable
        for (auto it = ds_data->required_descriptors_by_type.begin(); it != ds_data->required_descriptors_by_type.end(); ++it) {
            auto count_iter = pool_state->availableDescriptorTypeCount.find(it->first);
            uint32_t available_count = (count_iter != pool_state->availableDescriptorTypeCount.end()) ? count_iter->second : 0;

            if (ds_data->required_descriptors_by_type.at(it->first) > available_count) {
                skip |= LogError(pool_state->pool, "VUID-VkDescriptorSetAllocateInfo-descriptorPool-00307",
                                 "vkAllocateDescriptorSets(): Unable to allocate %u descriptors of type %s from %s"
                                 ". This pool only has %d descriptors of this type remaining.",
                                 ds_data->required_descriptors_by_type.at(it->first),
                                 string_VkDescriptorType(VkDescriptorType(it->first)),
                                 report_data->FormatHandle(pool_state->pool).c_str(), available_count);
            }
        }
    }

    const auto *count_allocate_info = LvlFindInChain<VkDescriptorSetVariableDescriptorCountAllocateInfo>(p_alloc_info->pNext);

    if (count_allocate_info) {
        if (count_allocate_info->descriptorSetCount != 0 &&
            count_allocate_info->descriptorSetCount != p_alloc_info->descriptorSetCount) {
            skip |= LogError(device, "VUID-VkDescriptorSetVariableDescriptorCountAllocateInfo-descriptorSetCount-03045",
                             "vkAllocateDescriptorSets(): VkDescriptorSetAllocateInfo::descriptorSetCount (%d) != "
                             "VkDescriptorSetVariableDescriptorCountAllocateInfo::descriptorSetCount (%d)",
                             p_alloc_info->descriptorSetCount, count_allocate_info->descriptorSetCount);
        }
        if (count_allocate_info->descriptorSetCount == p_alloc_info->descriptorSetCount) {
            for (uint32_t i = 0; i < p_alloc_info->descriptorSetCount; i++) {
                auto layout = GetDescriptorSetLayoutShared(p_alloc_info->pSetLayouts[i]);
                if (count_allocate_info->pDescriptorCounts[i] > layout->GetDescriptorCountFromBinding(layout->GetMaxBinding())) {
                    skip |= LogError(device, "VUID-VkDescriptorSetVariableDescriptorCountAllocateInfo-pSetLayouts-03046",
                                     "vkAllocateDescriptorSets(): pDescriptorCounts[%d] = (%d), binding's descriptorCount = (%d)",
                                     i, count_allocate_info->pDescriptorCounts[i],
                                     layout->GetDescriptorCountFromBinding(layout->GetMaxBinding()));
                }
            }
        }
    }

    return skip;
}

const BindingReqMap &cvdescriptorset::PrefilterBindRequestMap::FilteredMap(const CMD_BUFFER_STATE &cb_state,
                                                                           const PIPELINE_STATE &pipeline) {
    if (IsManyDescriptors()) {
        filtered_map_.reset(new BindingReqMap);
        descriptor_set_.FilterBindingReqs(cb_state, pipeline, orig_map_, filtered_map_.get());
        return *filtered_map_;
    }
    return orig_map_;
}

// Starting at offset descriptor of given binding, parse over update_count
//  descriptor updates and verify that for any binding boundaries that are crossed, the next binding(s) are all consistent
//  Consistency means that their type, stage flags, and whether or not they use immutable samplers matches
//  If so, return true. If not, fill in error_msg and return false
bool cvdescriptorset::VerifyUpdateConsistency(debug_report_data *report_data,
                                              DescriptorSetLayout::ConstBindingIterator current_binding, uint32_t offset,
                                              uint32_t update_count, const char *type, const VkDescriptorSet set,
                                              std::string *error_msg) {
    bool pass = true;
    // Verify consecutive bindings match (if needed)
    auto orig_binding = current_binding;

    while (pass && update_count) {
        // First, it's legal to offset beyond your own binding so handle that case
        if (offset > 0) {
            const auto &index_range = current_binding.GetGlobalIndexRange();
            // index_range.start + offset is which descriptor is needed to update. If it > index_range.end, it means the descriptor
            // isn't in this binding, maybe in next binding.
            if ((index_range.start + offset) >= index_range.end) {
                // Advance to next binding, decrement offset by binding size
                offset -= current_binding.GetDescriptorCount();
                ++current_binding;
                // Verify next consecutive binding matches type, stage flags & immutable sampler use and if AtEnd
                if (!orig_binding.IsConsistent(current_binding)) {
                    pass = false;
                }
                continue;
            }
        }

        update_count -= std::min(update_count, current_binding.GetDescriptorCount() - offset);
        if (update_count) {
            // Starting offset is beyond the current binding. Check consistency, update counters and advance to the next binding,
            // looking for the start point. All bindings (even those skipped) must be consistent with the update and with the
            // original binding.
            offset = 0;
            ++current_binding;
            // Verify next consecutive binding matches type, stage flags & immutable sampler use and if AtEnd
            if (!orig_binding.IsConsistent(current_binding)) {
                pass = false;
            }
        }
    }

    if (!pass) {
        std::stringstream error_str;
        error_str << "Attempting " << type;
        if (current_binding.Layout()->IsPushDescriptor()) {
            error_str << " push descriptors";
        } else {
            error_str << " descriptor set " << report_data->FormatHandle(set);
        }
        error_str << " binding #" << orig_binding.Binding() << " with #" << update_count
                  << " descriptors being updated but this update oversteps the bounds of this binding and the next binding is "
                     "not consistent with current binding so this update is invalid.";
        *error_msg = error_str.str();
    }
    return pass;
}

// Validate the state for a given write update but don't actually perform the update
//  If an error would occur for this update, return false and fill in details in error_msg string
bool CoreChecks::ValidateWriteUpdate(const DescriptorSet *dest_set, const VkWriteDescriptorSet *update, const char *func_name,
                                     std::string *error_code, std::string *error_msg) const {
    const auto dest_layout = dest_set->GetLayout().get();

    // Verify dst layout still valid
    if (dest_layout->destroyed) {
        *error_code = "VUID-VkWriteDescriptorSet-dstSet-00320";
        std::ostringstream str;
        str << "Cannot call " << func_name << " to perform write update on " << dest_set->StringifySetAndLayout()
            << " which has been destroyed";
        *error_msg = str.str();
        return false;
    }
    // Verify dst binding exists
    if (!dest_layout->HasBinding(update->dstBinding)) {
        *error_code = "VUID-VkWriteDescriptorSet-dstBinding-00315";
        std::stringstream error_str;
        error_str << dest_set->StringifySetAndLayout() << " does not have binding " << update->dstBinding;
        *error_msg = error_str.str();
        return false;
    }

    DescriptorSetLayout::ConstBindingIterator dest(dest_layout, update->dstBinding);
    // Make sure binding isn't empty
    if (0 == dest.GetDescriptorCount()) {
        *error_code = "VUID-VkWriteDescriptorSet-dstBinding-00316";
        std::stringstream error_str;
        error_str << dest_set->StringifySetAndLayout() << " cannot updated binding " << update->dstBinding
                  << " that has 0 descriptors";
        *error_msg = error_str.str();
        return false;
    }

    // Verify idle ds
    if (dest_set->in_use.load() && !(dest.GetDescriptorBindingFlags() & (VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
                                                                         VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT))) {
        // TODO : Re-using Free Idle error code, need write update idle error code
        *error_code = "VUID-vkFreeDescriptorSets-pDescriptorSets-00309";
        std::stringstream error_str;
        error_str << "Cannot call " << func_name << " to perform write update on " << dest_set->StringifySetAndLayout()
                  << " that is in use by a command buffer";
        *error_msg = error_str.str();
        return false;
    }
    // We know that binding is valid, verify update and do update on each descriptor
    auto start_idx = dest.GetGlobalIndexRange().start + update->dstArrayElement;
    auto type = dest.GetType();
    if (type != update->descriptorType) {
        *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00319";
        std::stringstream error_str;
        error_str << "Attempting write update to " << dest_set->StringifySetAndLayout() << " binding #" << update->dstBinding
                  << " with type " << string_VkDescriptorType(type) << " but update type is "
                  << string_VkDescriptorType(update->descriptorType);
        *error_msg = error_str.str();
        return false;
    }
    if (type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT) {
        if ((update->dstArrayElement % 4) != 0) {
            *error_code = "VUID-VkWriteDescriptorSet-descriptorType-02219";
            std::stringstream error_str;
            error_str << "Attempting write update to " << dest_set->StringifySetAndLayout() << " binding #" << update->dstBinding
                      << " with "
                      << "dstArrayElement " << update->dstArrayElement << " not a multiple of 4";
            *error_msg = error_str.str();
            return false;
        }
        if ((update->descriptorCount % 4) != 0) {
            *error_code = "VUID-VkWriteDescriptorSet-descriptorType-02220";
            std::stringstream error_str;
            error_str << "Attempting write update to " << dest_set->StringifySetAndLayout() << " binding #" << update->dstBinding
                      << " with "
                      << "descriptorCount  " << update->descriptorCount << " not a multiple of 4";
            *error_msg = error_str.str();
            return false;
        }
        const auto *write_inline_info = LvlFindInChain<VkWriteDescriptorSetInlineUniformBlockEXT>(update->pNext);
        if (!write_inline_info || write_inline_info->dataSize != update->descriptorCount) {
            *error_code = "VUID-VkWriteDescriptorSet-descriptorType-02221";
            std::stringstream error_str;
            if (!write_inline_info) {
                error_str << "Attempting write update to " << dest_set->StringifySetAndLayout() << " binding #"
                          << update->dstBinding << " with "
                          << "VkWriteDescriptorSetInlineUniformBlockEXT missing";
            } else {
                error_str << "Attempting write update to " << dest_set->StringifySetAndLayout() << " binding #"
                          << update->dstBinding << " with "
                          << "VkWriteDescriptorSetInlineUniformBlockEXT dataSize " << write_inline_info->dataSize
                          << " not equal to "
                          << "VkWriteDescriptorSet descriptorCount " << update->descriptorCount;
            }
            *error_msg = error_str.str();
            return false;
        }
        // This error is probably unreachable due to the previous two errors
        if (write_inline_info && (write_inline_info->dataSize % 4) != 0) {
            *error_code = "VUID-VkWriteDescriptorSetInlineUniformBlockEXT-dataSize-02222";
            std::stringstream error_str;
            error_str << "Attempting write update to " << dest_set->StringifySetAndLayout() << " binding #" << update->dstBinding
                      << " with "
                      << "VkWriteDescriptorSetInlineUniformBlockEXT dataSize " << write_inline_info->dataSize
                      << " not a multiple of 4";
            *error_msg = error_str.str();
            return false;
        }
    }
    // Verify all bindings update share identical properties across all items
    if (update->descriptorCount > 0) {
        // Save first binding information and error if something different is found
        DescriptorSetLayout::ConstBindingIterator current_binding(dest_layout, update->dstBinding);
        VkShaderStageFlags stage_flags = current_binding.GetStageFlags();
        VkDescriptorType descriptor_type = current_binding.GetType();
        bool immutable_samplers = (current_binding.GetImmutableSamplerPtr() == nullptr);
        uint32_t dst_array_element = update->dstArrayElement;

        for (uint32_t i = 0; i < update->descriptorCount;) {
            if (current_binding.AtEnd() == true) {
                break;  // prevents setting error here if bindings don't exist
            }

            // Check for consistent stageFlags and descriptorType
            if ((current_binding.GetStageFlags() != stage_flags) || (current_binding.GetType() != descriptor_type)) {
                *error_code = "VUID-VkWriteDescriptorSet-descriptorCount-00317";
                std::stringstream error_str;
                error_str << "Attempting write update to " << dest_set->StringifySetAndLayout() << " binding index #"
                          << current_binding.GetIndex() << " (" << i << " from dstBinding offset)"
                          << " with a different stageFlag and/or descriptorType from previous bindings."
                          << " All bindings must have consecutive stageFlag and/or descriptorType across a VkWriteDescriptorSet";
                *error_msg = error_str.str();
                return false;
            }
            // Check if all immutableSamplers or not
            if ((current_binding.GetImmutableSamplerPtr() == nullptr) != immutable_samplers) {
                *error_code = "VUID-VkWriteDescriptorSet-descriptorCount-00318";
                std::stringstream error_str;
                error_str << "Attempting write update to " << dest_set->StringifySetAndLayout() << " binding index #"
                          << current_binding.GetIndex() << " (" << i << " from dstBinding offset)"
                          << " with a different usage of immutable samplers from previous bindings."
                          << " All bindings must have all or none usage of immutable samplers across a VkWriteDescriptorSet";
                *error_msg = error_str.str();
                return false;
            }

            // Skip the remaining descriptors for this binding, and move to the next binding
            i += (current_binding.GetDescriptorCount() - dst_array_element);
            dst_array_element = 0;
            ++current_binding;
        }
    }

    // Verify consecutive bindings match (if needed)
    if (!VerifyUpdateConsistency(report_data, DescriptorSetLayout::ConstBindingIterator(dest_layout, update->dstBinding),
                                 update->dstArrayElement, update->descriptorCount, "write update to", dest_set->GetSet(),
                                 error_msg)) {
        // TODO : Should break out "consecutive binding updates" language into valid usage statements
        *error_code = "VUID-VkWriteDescriptorSet-dstArrayElement-00321";
        return false;
    }
    // Verify write to variable descriptor
    if (dest_set->IsVariableDescriptorCount(update->dstBinding)) {
        if ((update->dstArrayElement + update->descriptorCount) > dest_set->GetVariableDescriptorCount()) {
            std::stringstream error_str;
            *error_code = "VUID-VkWriteDescriptorSet-dstArrayElement-00321";
            error_str << "Attempting write update to " << dest_set->StringifySetAndLayout() << " binding index #"
                      << update->dstBinding << " array element " << update->dstArrayElement << " with " << update->descriptorCount
                      << " writes but variable descriptor size is " << dest_set->GetVariableDescriptorCount();
            *error_msg = error_str.str();
            return false;
        }
    }
    // Update is within bounds and consistent so last step is to validate update contents
    if (!VerifyWriteUpdateContents(dest_set, update, start_idx, func_name, error_code, error_msg)) {
        std::stringstream error_str;
        error_str << "Write update to " << dest_set->StringifySetAndLayout() << " binding #" << update->dstBinding
                  << " failed with error message: " << error_msg->c_str();
        *error_msg = error_str.str();
        return false;
    }
    // All checks passed, update is clean
    return true;
}

// Verify that the contents of the update are ok, but don't perform actual update
bool CoreChecks::VerifyWriteUpdateContents(const DescriptorSet *dest_set, const VkWriteDescriptorSet *update, const uint32_t index,
                                           const char *func_name, std::string *error_code, std::string *error_msg) const {
    using ImageSamplerDescriptor = cvdescriptorset::ImageSamplerDescriptor;
    using Descriptor = cvdescriptorset::Descriptor;

    switch (update->descriptorType) {
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
            for (uint32_t di = 0; di < update->descriptorCount; ++di) {
                // Validate image
                auto image_view = update->pImageInfo[di].imageView;
                auto image_layout = update->pImageInfo[di].imageLayout;
                auto sampler = update->pImageInfo[di].sampler;
                auto iv_state = GetImageViewState(image_view);
                const ImageSamplerDescriptor *desc =
                    (const ImageSamplerDescriptor *)dest_set->GetDescriptorFromGlobalIndex(index + di);
                if (image_view) {
                    auto image_state = iv_state->image_state.get();
                    if (!ValidateImageUpdate(image_view, image_layout, update->descriptorType, func_name, error_code, error_msg)) {
                        std::stringstream error_str;
                        error_str << "Attempted write update to combined image sampler descriptor failed due to: "
                                  << error_msg->c_str();
                        *error_msg = error_str.str();
                        return false;
                    }
                    if (device_extensions.vk_khr_sampler_ycbcr_conversion) {
                        if (desc->IsImmutableSampler()) {
                            auto sampler_state = GetSamplerState(desc->GetSampler());
                            if (iv_state && sampler_state) {
                                if (iv_state->samplerConversion != sampler_state->samplerConversion) {
                                    *error_code = "VUID-VkWriteDescriptorSet-descriptorType-01948";
                                    std::stringstream error_str;
                                    error_str
                                        << "Attempted write update to combined image sampler and image view and sampler ycbcr "
                                           "conversions are not identical, sampler: "
                                        << report_data->FormatHandle(desc->GetSampler())
                                        << " image view: " << report_data->FormatHandle(iv_state->image_view) << ".";
                                    *error_msg = error_str.str();
                                    return false;
                                }
                            }
                        } else {
                            if (iv_state && (iv_state->samplerConversion != VK_NULL_HANDLE)) {
                                *error_code = "VUID-VkWriteDescriptorSet-descriptorType-02738";
                                std::stringstream error_str;
                                error_str << "Because dstSet (" << report_data->FormatHandle(update->dstSet)
                                          << ") is bound to image view (" << report_data->FormatHandle(iv_state->image_view)
                                          << ") that includes a YCBCR conversion, it must have been allocated with a layout that "
                                             "includes an immutable sampler.";
                                *error_msg = error_str.str();
                                return false;
                            }
                        }
                    }
                    // If there is an immutable sampler then |sampler| isn't used, so the following VU does not apply.
                    if (sampler && !desc->IsImmutableSampler() && FormatIsMultiplane(image_state->createInfo.format)) {
                        // multiplane formats must be created with mutable format bit
                        if (0 == (image_state->createInfo.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) {
                            *error_code = "VUID-VkDescriptorImageInfo-sampler-01564";
                            std::stringstream error_str;
                            error_str << "image " << report_data->FormatHandle(image_state->image)
                                      << " combined image sampler is a multi-planar "
                                      << "format and was not was not created with the VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT";
                            *error_msg = error_str.str();
                            return false;
                        }
                        // image view need aspect mask for only the planes supported of format
                        VkImageAspectFlags legal_aspect_flags = (VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT);
                        legal_aspect_flags |=
                            (FormatPlaneCount(image_state->createInfo.format) == 3) ? VK_IMAGE_ASPECT_PLANE_2_BIT : 0;
                        if (0 != (iv_state->create_info.subresourceRange.aspectMask & (~legal_aspect_flags))) {
                            *error_code = "VUID-VkDescriptorImageInfo-sampler-01564";
                            std::stringstream error_str;
                            error_str << "image " << report_data->FormatHandle(image_state->image)
                                      << " combined image sampler is a multi-planar "
                                      << "format and " << report_data->FormatHandle(iv_state->image_view)
                                      << " aspectMask must only include " << string_VkImageAspectFlags(legal_aspect_flags);
                            *error_msg = error_str.str();
                            return false;
                        }
                    }

                    // Verify portability
                    auto sampler_state = GetSamplerState(sampler);
                    if (sampler_state) {
                        if (ExtEnabled::kNotEnabled != device_extensions.vk_khr_portability_subset) {
                            if ((VK_FALSE == enabled_features.portability_subset_features.mutableComparisonSamplers) &&
                                (VK_FALSE != sampler_state->createInfo.compareEnable)) {
                                LogError(device, "VUID-VkDescriptorImageInfo-mutableComparisonSamplers-04450",
                                         "%s (portability error): sampler comparison not available.", func_name);
                            }
                        }
                    }
                }
            }
        }
        // Fall through
        case VK_DESCRIPTOR_TYPE_SAMPLER: {
            for (uint32_t di = 0; di < update->descriptorCount; ++di) {
                const auto *desc = static_cast<const Descriptor *>(dest_set->GetDescriptorFromGlobalIndex(index + di));
                if (!desc->IsImmutableSampler()) {
                    if (!ValidateSampler(update->pImageInfo[di].sampler)) {
                        *error_code = "VUID-VkWriteDescriptorSet-descriptorType-00325";
                        std::stringstream error_str;
                        error_str << "Attempted write update to sampler descriptor with invalid sampler: "
                                  << report_data->FormatHandle(update->pImageInfo[di].sampler) << ".";
                        *error_msg = error_str.str();
                        return false;
                    }
                } else if (update->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) {
                    *error_code = "VUID-VkWriteDescriptorSet-descriptorType-02752";
                    std::stringstream error_str;
                    error_str << "Attempted write update to an immutable sampler descriptor.";
                    *error_msg = error_str.str();
                    return false;
                }
            }
            break;
        }
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
            for (uint32_t di = 0; di < update->descriptorCount; ++di) {
                auto image_view = update->pImageInfo[di].imageView;
                auto image_layout = update->pImageInfo[di].imageLayout;
                if (image_view) {
                    if (!ValidateImageUpdate(image_view, image_layout, update->descriptorType, func_name, error_code, error_msg)) {
                        std::stringstream error_str;
                        error_str << "Attempted write update to image descriptor failed due to: " << error_msg->c_str();
                        *error_msg = error_str.str();
                        return false;
                    }
                }
            }
            break;
        }
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
            for (uint32_t di = 0; di < update->descriptorCount; ++di) {
                auto buffer_view = update->pTexelBufferView[di];
                if (buffer_view) {
                    auto bv_state = GetBufferViewState(buffer_view);
                    if (!bv_state) {
                        *error_code = "VUID-VkWriteDescriptorSet-descriptorType-02994";
                        std::stringstream error_str;
                        error_str << "Attempted write update to texel buffer descriptor with invalid buffer view: "
                                  << report_data->FormatHandle(buffer_view);
                        *error_msg = error_str.str();
                        return false;
                    }
                    auto buffer = bv_state->create_info.buffer;
                    auto buffer_state = GetBufferState(buffer);
                    // Verify that buffer underlying the view hasn't been destroyed prematurely
                    if (!buffer_state) {
                        *error_code = "VUID-VkWriteDescriptorSet-descriptorType-02994";
                        std::stringstream error_str;
                        error_str << "Attempted write update to texel buffer descriptor failed because underlying buffer ("
                                  << report_data->FormatHandle(buffer) << ") has been destroyed: " << error_msg->c_str();
                        *error_msg = error_str.str();
                        return false;
                    } else if (!cvdescriptorset::ValidateBufferUsage(report_data, buffer_state, update->descriptorType, error_code,
                                                                     error_msg)) {
                        std::stringstream error_str;
                        error_str << "Attempted write update to texel buffer descriptor failed due to: " << error_msg->c_str();
                        *error_msg = error_str.str();
                        return false;
                    }
                }
            }
            break;
        }
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
            for (uint32_t di = 0; di < update->descriptorCount; ++di) {
                if (update->pBufferInfo[di].buffer) {
                    if (!ValidateBufferUpdate(update->pBufferInfo + di, update->descriptorType, func_name, error_code, error_msg)) {
                        std::stringstream error_str;
                        error_str << "Attempted write update to buffer descriptor failed due to: " << error_msg->c_str();
                        *error_msg = error_str.str();
                        return false;
                    }
                }
            }
            break;
        }
        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT:
            break;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV: {
            const auto *acc_info = LvlFindInChain<VkWriteDescriptorSetAccelerationStructureNV>(update->pNext);
            for (uint32_t di = 0; di < update->descriptorCount; ++di) {
                if (!ValidateAccelerationStructureUpdate(GetAccelerationStructureStateNV(acc_info->pAccelerationStructures[di]),
                                                         func_name, error_code, error_msg)) {
                    std::stringstream error_str;
                    error_str << "Attempted write update to acceleration structure descriptor failed due to: "
                              << error_msg->c_str();
                    *error_msg = error_str.str();
                    return false;
                }
            }

        } break;
        // KHR acceleration structures don't require memory to be bound manually to them.
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            break;
        default:
            assert(0);  // We've already verified update type so should never get here
            break;
    }
    // All checks passed so update contents are good
    return true;
}
