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
    Renderer(FrameGeneration mode, bool upscaling)
        : mode(mode), predictionEnabled(mode != FrameGeneration::Off), upscaling(upscaling) {}
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
    void initializeTiming(brls::Switch2VulkanContext& ctx);
    void collectTiming(brls::Switch2VulkanContext& ctx);
    // A picture's luma upscaled by easu.comp, kept while the picture is drawn.
    struct Upscaled
    {
        brls::Switch2VkBuffer buffer;
        std::weak_ptr<DecoderPool> pool;
        uint64_t sequence = 0, used = 0;
        uint32_t width = 0, height = 0;
    };
    Upscaled* findUpscaled(const FramePtr& frame, uint32_t width, uint32_t height);
    Upscaled* claimUpscaled(brls::Switch2VulkanContext& ctx, const FramePtr& frame, uint32_t width,
        uint32_t height, const Upscaled* keep);

    FrameGeneration mode;
    bool predictionEnabled;
    VkPipeline flowPipeline = VK_NULL_HANDLE, predictPipeline = VK_NULL_HANDLE;
    // Compute ahead of the frame: EASU for a new picture, motion estimation.
    std::array<VkCommandBuffer, 2> computeCommands {};
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
    bool upscaling;
    int lastEasu = -1; // logged when EASU turns on or off
    VkPipeline easuPipeline = VK_NULL_HANDLE;
    // Four pictures: the draws in flight use at most the last three.
    std::array<Upscaled, 4> upscaledCache {};
    uint64_t upscaledUses = 0;
    brls::Switch2VkBuffer placeholder {}; // bound where no upscaled luma is used
    // GPU time of the video draw: two timestamps per frame slot, read back
    // when the slot comes round again (its fence has been waited for).
    VkQueryPool timestamps = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 2> resetCommands {};
    std::array<bool, 2> timed {}, easuTimed {};
    double timestampNs = 0;
    uint64_t timingStartUs = 0;
    uint32_t timedDraws = 0;
    double drawSumMs = 0, drawMaxMs = 0;
    uint32_t easuRuns = 0;
    double easuSumMs = 0, easuMaxMs = 0;
};

}
