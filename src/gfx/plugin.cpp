// FH2 native renderer plugin (librexgpu-fh2.so, --gpu_plugin=fh2).
//
// src/gfx/vk is a fork of the SDK's Xenia-derived Vulkan backend
// (namespace rex::graphics::fh2vk). It starts as a working renderer and is
// replaced subsystem by subsystem with FH2-specific code: render targets as
// host images without EDRAM emulation, resolves into host textures,
// precompiled shader packs, lower per-draw overhead, graphics options.
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#include "vk/graphics_system.h"

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion || !info ||
      info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-fh2: ABI or create-info mismatch");
    return nullptr;
  }
  REXLOG_INFO("rexgpu-fh2: FH2 native renderer");
  return new rex::graphics::fh2vk::VulkanGraphicsSystem();
}
