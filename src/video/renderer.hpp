// Draws decoded NV12 frames into Borealis's Vulkan frame.
// Adapted from Moonlight-Switch (XITRIX, GPL-3.0): VulkanVideoRenderer.
#pragma once

#include "decoded_frame.hpp"

#include <array>
#include <borealis/platforms/switch/switch2_vulkan.hpp>

namespace ounce
{

class Renderer
{
  public:
    ~Renderer();
    // Records the draw of frame into the current Vulkan frame; the frame is
    // kept alive until the GPU has finished with it.
    void draw(const FramePtr& frame);

  private:
    void initialize();

    VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
    VkDescriptorPool pool             = VK_NULL_HANDLE;
    VkPipelineLayout layout           = VK_NULL_HANDLE;
    VkPipeline pipeline               = VK_NULL_HANDLE;
    VkPipeline texelPipeline          = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> sets {};
    std::array<brls::Switch2VkBuffer, 2> uploads {};
    int lastSource = -1;
};

}
