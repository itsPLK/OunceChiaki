// Draws decoded NV12 frames into Borealis's Vulkan frame.
// Adapted from Moonlight-Switch (XITRIX, GPL-3.0): VulkanVideoRenderer.
#pragma once

#include "decoded_frame.hpp"
#include "frame_generation.hpp"

#include <array>
#include <borealis/platforms/switch/switch2_vulkan.hpp>

namespace ounce
{

class Renderer
{
  public:
    explicit Renderer(FrameGeneration mode) : mode(mode), predictionEnabled(mode != FrameGeneration::Off) {}
    ~Renderer();
    // Records the draw of frame into the current Vulkan frame; the frame is
    // kept alive until the GPU has finished with it.
    // Returns true if the draw used the previous frame to predict motion.
    bool draw(const FramePtr& frame, const FramePtr& previous = {});
    bool canPredict() const { return flowPipeline && predictPipeline && lastSource >= 2; }

  private:
    void initialize();

    FrameGeneration mode;
    bool predictionEnabled;
    VkPipeline flowPipeline = VK_NULL_HANDLE, predictPipeline = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 2> flowCommands {};
    std::array<brls::Switch2VkBuffer, 2> motion {};
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
