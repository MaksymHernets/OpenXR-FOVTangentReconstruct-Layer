#pragma once
#include "common/Platform.h"

// Experimental developer cooperation API, versioned independently of OpenXR.
// These commands are obtained through this layer's xrGetInstanceProcAddr; they
// are not a registered OpenXR extension. See docs/VULKAN_SYNCHRONIZATION.md.
// Hold the session queue's external lock around OpenXR release/end-frame calls.
// The application must have initialized its Vulkan instance/device for 1.1+;
// physical-device support alone does not prove the application's enabled API.
constexpr uint32_t FOV_VULKAN_COOPERATION_VERSION = 1;
using PFN_xrFovEnableVulkanProcessing = XrResult(XRAPI_PTR *)(XrSession, uint32_t);
// The completed fence must cover all image writes and ownership restoration on
// producerQueue. No further image access is allowed before successful release.
using PFN_xrFovDeclareVulkanImageState = XrResult(XRAPI_PTR *)(XrSwapchain, uint32_t, VkImageLayout, VkQueue,
                                                               VkFence);
