#include "fh2_gfx.h"

#include <cmath>

#include <xxhash.h>

#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

namespace fh2::gfx {

using namespace rex::graphics;

// ---------------------------------------------------------------------------
// Graphics system

void Fh2GraphicsSystem::CreateProvider(bool with_presentation) {
  provider_ = rex::ui::vulkan::VulkanProvider::Create(true, with_presentation);
}

std::unique_ptr<CommandProcessor> Fh2GraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<Fh2CommandProcessor>(this, kernel_state_);
}

// ---------------------------------------------------------------------------
// Command processor

Fh2CommandProcessor::Fh2CommandProcessor(Fh2GraphicsSystem* graphics_system,
                                         rex::system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

Fh2CommandProcessor::~Fh2CommandProcessor() = default;

const rex::ui::vulkan::VulkanDevice* Fh2CommandProcessor::device() const {
  return static_cast<const rex::ui::vulkan::VulkanProvider*>(graphics_system_->provider())
      ->vulkan_device();
}

bool Fh2CommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    return false;
  }
  const auto* vk = device();
  const auto& dfn = vk->functions();
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = vk->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(vk->device(), &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
    REXGPU_ERROR("fh2gfx: failed to create the command pool");
    return false;
  }
  VkCommandBufferAllocateInfo alloc_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc_info.commandPool = command_pool_;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(vk->device(), &alloc_info, &command_buffer_) != VK_SUCCESS) {
    REXGPU_ERROR("fh2gfx: failed to allocate a command buffer");
    return false;
  }
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (dfn.vkCreateFence(vk->device(), &fence_info, nullptr, &fence_) != VK_SUCCESS) {
    REXGPU_ERROR("fh2gfx: failed to create a fence");
    return false;
  }
  REXGPU_INFO("fh2gfx: native renderer skeleton ready (dynamic rendering: {})",
              vk->properties().dynamicRendering);
  return true;
}

void Fh2CommandProcessor::ShutdownContext() {
  const auto* vk = device();
  if (vk) {
    const auto& dfn = vk->functions();
    if (fence_) dfn.vkDestroyFence(vk->device(), fence_, nullptr);
    if (command_pool_) dfn.vkDestroyCommandPool(vk->device(), command_pool_, nullptr);
  }
  fence_ = VK_NULL_HANDLE;
  command_pool_ = VK_NULL_HANDLE;
  command_buffer_ = VK_NULL_HANDLE;
  shaders_.clear();
  CommandProcessor::ShutdownContext();
}

Shader* Fh2CommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                        const uint32_t* host_address, uint32_t dword_count) {
  (void)guest_address;
  // Same key as the shader pack: XXH3 of the big-endian microcode.
  uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<Shader>(shader_type, hash, host_address, dword_count);
  Shader* result = shader.get();
  shaders_.emplace(hash, std::move(shader));
  return result;
}

bool Fh2CommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                    IndexBufferInfo* index_buffer_info,
                                    bool major_mode_explicit) {
  (void)prim_type;
  (void)index_count;
  (void)index_buffer_info;
  (void)major_mode_explicit;
  ++draws_this_frame_;
  return true;
}

bool Fh2CommandProcessor::IssueCopy() {
  ++copies_this_frame_;
  return true;
}

void Fh2CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                    uint32_t frontbuffer_height) {
  (void)frontbuffer_ptr;
  ++frame_;
  if (frame_ % 300 == 1) {
    REXGPU_INFO("fh2gfx: frame {} -- {} draws, {} copies, {} shaders loaded, front {}x{}", frame_,
                draws_this_frame_, copies_this_frame_, shaders_.size(), frontbuffer_width,
                frontbuffer_height);
  }
  draws_this_frame_ = 0;
  copies_this_frame_ = 0;

  rex::ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter || !frontbuffer_width || !frontbuffer_height) {
    return;
  }
  const auto* vk = device();
  const auto& dfn = vk->functions();
  // Slowly cycling colour: proof that frames flow through the native path.
  float t = float(frame_) / 120.0f;
  VkClearColorValue clear_color{{0.5f + 0.5f * std::sin(t), 0.2f, 0.5f + 0.5f * std::cos(t), 1.0f}};

  presenter->RefreshGuestOutput(
      frontbuffer_width, frontbuffer_height, 1280, 720,
      [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vk_context =
            static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        dfn.vkResetCommandPool(vk->device(), command_pool_, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        dfn.vkBeginCommandBuffer(command_buffer_, &begin);

        VkImageMemoryBarrier to_attachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_attachment.srcAccessMask = vk_context.image_ever_written_previously()
                                          ? rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask
                                          : 0;
        to_attachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        to_attachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_attachment.image = vk_context.image();
        to_attachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dfn.vkCmdPipelineBarrier(
            command_buffer_,
            vk_context.image_ever_written_previously()
                ? VkPipelineStageFlags(rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask)
                : VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &to_attachment);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = vk_context.image_view();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = clear_color;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {frontbuffer_width, frontbuffer_height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        dfn.vkCmdBeginRendering(command_buffer_, &rendering);
        dfn.vkCmdEndRendering(command_buffer_);

        VkImageMemoryBarrier to_shader_read = to_attachment;
        to_shader_read.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        to_shader_read.dstAccessMask = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
        to_shader_read.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_shader_read.newLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
        dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask, 0,
                                 0, nullptr, 0, nullptr, 1, &to_shader_read);
        dfn.vkEndCommandBuffer(command_buffer_);

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer_;
        {
          auto queue = vk->AcquireQueue(vk->queue_family_graphics_compute(), 0);
          if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence_) != VK_SUCCESS) {
            return false;
          }
        }
        dfn.vkWaitForFences(vk->device(), 1, &fence_, VK_TRUE, UINT64_MAX);
        dfn.vkResetFences(vk->device(), 1, &fence_);
        return true;
      });
}

}  // namespace fh2::gfx

// ---------------------------------------------------------------------------
// Plugin entry points

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
  return new fh2::gfx::Fh2GraphicsSystem();
}
