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
    // When previous is given, estimates motion and draws a generated picture.
    // When reuseFlow is true (extrapolation only), reuses the last motion field
    // for this frame without dispatching compute again, so keepalive repeats
    // hold the extrapolated picture instead of snapping back to the real frame.
    // Returns true if the draw used the prediction pipeline.
    bool draw(const FramePtr& frame, const FramePtr& previous = {}, bool reuseFlow = false);
    bool canPredict() const { return flowPipeline && predictPipeline && lastSource >= 2; }

  private:
    void initialize();

    FrameGeneration mode;
    bool predictionEnabled;
    VkPipeline flowPipeline = VK_NULL_HANDLE, predictPipeline = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 2> flowCommands {};
    std::array<brls::Switch2VkBuffer, 2> motion {};
    const DecodedFrame* flowFrame = nullptr;
    size_t flowSlot               = 0;
    VkDeviceSize flowSize         = 0;
    uint32_t flowWidth = 0, flowHeight = 0;
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
