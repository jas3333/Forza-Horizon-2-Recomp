// FH2 native renderer: a GPU plugin (librexgpu-fh2.so, --gpu_plugin=fh2)
// that consumes the game's PM4 command stream with the SDK's packet parser
// and renders it with its own Vulkan code -- no EDRAM emulation, shaders from
// the precompiled SPIR-V pack. See the native-renderer plan in the repo.
//
// Skeleton stage: swaps present a cleared frame, draws are only counted.
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/ui/vulkan/device.h>

namespace fh2::gfx {

class Fh2GraphicsSystem final : public rex::graphics::GraphicsSystem {
 public:
  std::string name() const override { return "FH2 native"; }

 protected:
  void CreateProvider(bool with_presentation) override;
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override;
};

class Fh2CommandProcessor final : public rex::graphics::CommandProcessor {
 public:
  Fh2CommandProcessor(Fh2GraphicsSystem* graphics_system, rex::system::KernelState* kernel_state);
  ~Fh2CommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType shader_type,
                                    uint32_t guest_address, const uint32_t* host_address,
                                    uint32_t dword_count) override;
  bool IssueDraw(rex::graphics::xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  const rex::ui::vulkan::VulkanDevice* device() const;

  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;

  std::unordered_map<uint64_t, std::unique_ptr<rex::graphics::Shader>> shaders_;
  uint64_t frame_ = 0;
  uint64_t draws_this_frame_ = 0;
  uint64_t copies_this_frame_ = 0;
};

}  // namespace fh2::gfx
