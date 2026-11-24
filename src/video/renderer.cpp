// Adapted from Moonlight-Switch (XITRIX, GPL-3.0): VulkanVideoRenderer.cpp.
// initialize() and the destructor are Moonlight's; draw() takes our frames.
#include "renderer.hpp"

#include "image_interop.hpp"
#include "video_spirv.hpp"

#include <borealis.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace ounce
{

namespace
{

struct Params
{
    uint32_t plane[4], extra[4];
    float region[4], red[4], green[4], blue[4];
    uint32_t previousChroma[2];
    uint32_t upscaledSize[2];
};
static_assert(sizeof(Params) == 112);
VkShaderModule shader(brls::Switch2VulkanContext& ctx, const uint32_t* code, size_t size)
{
    VkShaderModuleCreateInfo info { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = size;
    info.pCode    = code;
    VkShaderModule result;
    ctx.check(vkCreateShaderModule(ctx.device, &info, nullptr, &result), "video shader");
    return result;
}

struct Dispatch
{
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0; // invocations, 8x8 per group
};

// Borealis has already opened the UI render pass, so compute (EASU for a new
// picture, motion estimation) goes ahead of it in a submit of its own on the
// same queue. EASU is timed when queries are given (reset by collectTiming).
void submitCompute(brls::Switch2VulkanContext& ctx, VkCommandBuffer cmd, VkPipelineLayout layout,
    VkDescriptorSet set, const Params& params, VkShaderStageFlags stagesUsed, Dispatch easu, Dispatch flow,
    VkQueryPool queries, uint32_t easuQuery)
{
    ctx.check(vkResetCommandBuffer(cmd, 0), "reset compute commands");
    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ctx.check(vkBeginCommandBuffer(cmd, &begin), "begin compute");
    VkMemoryBarrier barrier { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout, stagesUsed, 0, sizeof(params), &params);
    for (const Dispatch& work : { easu, flow })
    {
        if (!work.pipeline)
            continue;
        const bool timed = queries && work.pipeline == easu.pipeline;
        if (timed)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, easuQuery);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, work.pipeline);
        vkCmdDispatch(cmd, (work.width + 7) / 8, (work.height + 7) / 8, 1);
        if (timed)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, easuQuery + 1);
    }
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
    ctx.check(vkEndCommandBuffer(cmd), "end compute");
    VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    std::lock_guard lock(ctx.queueMutex);
    ctx.check(vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE), "submit compute");
}

}

void Renderer::initialize()
{
    auto& ctx = *brls::Switch2VulkanContext::current();
    if (predictionEnabled || upscaling)
    {
        uint32_t count = 0;
        if (vkEnumerateDeviceExtensionProperties(ctx.physical, nullptr, &count, nullptr) == VK_SUCCESS && count)
        {
            std::vector<VkExtensionProperties> extensions(count);
            if (vkEnumerateDeviceExtensionProperties(ctx.physical, nullptr, &count, extensions.data()) == VK_SUCCESS)
            {
                bool opticalFlow = false;
                for (const auto& ext : extensions)
                    opticalFlow |= std::strcmp(ext.extensionName, "VK_NV_optical_flow") == 0;
                brls::Logger::info("Frame generation: VK_NV_optical_flow advertised={}", opticalFlow);
            }
        }
        count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical, &count, families.data());
        if (!(families[ctx.queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT))
        {
            brls::Logger::warning("Renderer: graphics queue lacks compute support; no frame generation or upscaling");
            predictionEnabled = upscaling = false;
        }
    }
    std::vector<VkDescriptorSetLayoutBinding> bindings {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
    };
    if (!ctx.texelBufferSampling)
        bindings.resize(1);
    VkShaderStageFlags stagesUsed = VK_SHADER_STAGE_FRAGMENT_BIT;
    if (predictionEnabled || upscaling)
        stagesUsed |= VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[0].stageFlags = stagesUsed;
    if (predictionEnabled)
    {
        bindings.push_back({ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stagesUsed, nullptr });
        bindings.push_back({ 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stagesUsed, nullptr });
    }
    // Upscaled luma: the picture's (written by easu.comp) and, for
    // interpolation, the earlier picture's.
    bindings.push_back({ 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stagesUsed, nullptr });
    bindings.push_back({ 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
    VkDescriptorSetLayoutCreateInfo descriptorInfo { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    descriptorInfo.bindingCount = bindings.size();
    descriptorInfo.pBindings    = bindings.data();
    ctx.check(vkCreateDescriptorSetLayout(ctx.device, &descriptorInfo, nullptr, &descriptors), "video descriptors");
    VkPushConstantRange constants { stagesUsed, 0, sizeof(Params) };
    VkPipelineLayoutCreateInfo layoutInfo { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &descriptors;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &constants;
    ctx.check(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &layout), "video layout");
    VkDescriptorPoolSize sizes[] { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, predictionEnabled ? 10u : 6u },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 4 } };
    VkDescriptorPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets       = 2;
    poolInfo.poolSizeCount = ctx.texelBufferSampling ? 2 : 1;
    poolInfo.pPoolSizes    = sizes;
    ctx.check(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &pool), "video pool");
    VkDescriptorSetLayout layouts[] { descriptors, descriptors };
    VkDescriptorSetAllocateInfo allocation { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocation.descriptorPool     = pool;
    allocation.descriptorSetCount = sets.size();
    allocation.pSetLayouts        = layouts;
    ctx.check(vkAllocateDescriptorSets(ctx.device, &allocation, sets.data()), "video sets");
    const VkShaderModule vert = shader(ctx, video_vert, sizeof(video_vert)),
                         frag = shader(ctx, video_frag, sizeof(video_frag));
    VkPipelineShaderStageCreateInfo stages[2] { };
    for (auto& stage : stages)
    {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    VkPipelineVertexInputStateCreateInfo vertex { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth   = 1;
    VkPipelineMultisampleStateCreateInfo msaa { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState color { };
    color.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1;
    blend.pAttachments    = &color;
    VkDynamicState dynamicStates[] { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates    = dynamicStates;
    VkGraphicsPipelineCreateInfo info { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState      = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &msaa;
    info.pDepthStencilState  = &depth;
    info.pColorBlendState    = &blend;
    info.pDynamicState       = &dynamic;
    info.layout              = layout;
    info.renderPass          = ctx.renderPass;
    auto rc                  = vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (rc == VK_SUCCESS && ctx.texelBufferSampling)
    {
        const VkShaderModule texel = shader(ctx, video_texel_frag, sizeof(video_texel_frag));
        stages[1].module           = texel;
        const VkResult texelResult
            = vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &texelPipeline);
        vkDestroyShaderModule(ctx.device, texel, nullptr);
        if (texelResult != VK_SUCCESS)
            brls::Logger::warning("SWITCH2_VULKAN_VIDEO: texel pipeline failed rc={}; direct storage sampling retained",
                int(texelResult));
    }
    if (rc == VK_SUCCESS && predictionEnabled)
    {
        const auto comp = shader(ctx, video_flow, sizeof(video_flow));
        VkComputePipelineCreateInfo compute { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        compute.layout = layout;
        compute.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = comp;
        compute.stage.pName = "main";
        auto result = vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &compute, nullptr, &flowPipeline);
        vkDestroyShaderModule(ctx.device, comp, nullptr);
        if (result == VK_SUCCESS)
        {
            const auto predict = mode == FrameGeneration::Interpolate
                ? shader(ctx, video_midpoint_frag, sizeof(video_midpoint_frag))
                : shader(ctx, video_predict_frag, sizeof(video_predict_frag));
            stages[1].module = predict;
            result = vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &predictPipeline);
            vkDestroyShaderModule(ctx.device, predict, nullptr);
        }
        if (result != VK_SUCCESS)
            brls::Logger::warning("Frame generation: pipeline creation failed rc={}", int(result));
    }
    if (rc == VK_SUCCESS && upscaling)
    {
        const auto comp = shader(ctx, video_easu, sizeof(video_easu));
        VkComputePipelineCreateInfo compute { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        compute.layout       = layout;
        compute.stage        = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        compute.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = comp;
        compute.stage.pName  = "main";
        const auto result = vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &compute, nullptr, &easuPipeline);
        vkDestroyShaderModule(ctx.device, comp, nullptr);
        if (result != VK_SUCCESS)
        {
            brls::Logger::warning("Renderer: EASU pipeline creation failed rc={}; bilinear upscaling", int(result));
            easuPipeline = VK_NULL_HANDLE;
        }
    }
    if (predictPipeline || easuPipeline)
    {
        VkCommandBufferAllocateInfo alloc { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc.commandPool = ctx.commandPool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = computeCommands.size();
        ctx.check(vkAllocateCommandBuffers(ctx.device, &alloc, computeCommands.data()), "compute commands");
    }
    placeholder = ctx.createBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
    ctx.check(rc, "video pipeline");
    initializeTiming(ctx);
}

Renderer::Upscaled* Renderer::findUpscaled(const FramePtr& frame, uint32_t width, uint32_t height)
{
    if (!frame)
        return nullptr;
    for (auto& entry : upscaledCache)
        if (entry.buffer.buffer && entry.sequence == frame->sequence && entry.width == width
            && entry.height == height && entry.pool.lock() == frame->pool)
            return &entry;
    return nullptr;
}

Renderer::Upscaled* Renderer::claimUpscaled(brls::Switch2VulkanContext& ctx, const FramePtr& frame,
    uint32_t width, uint32_t height, const Upscaled* keep)
{
    // The least recently used entry: draws still in flight read only the
    // pictures of the last two frames (at most three entries).
    Upscaled* oldest = nullptr;
    for (auto& entry : upscaledCache)
        if (&entry != keep && (!oldest || entry.used < oldest->used))
            oldest = &entry;
    const VkDeviceSize bytes = VkDeviceSize((width + 3) / 4) * height * 4;
    if (oldest->buffer.size < bytes)
    {
        ctx.destroyBuffer(oldest->buffer);
        oldest->buffer = ctx.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    oldest->pool     = frame->pool;
    oldest->sequence = frame->sequence;
    oldest->width    = width;
    oldest->height   = height;
    return oldest;
}

void Renderer::initializeTiming(brls::Switch2VulkanContext& ctx)
{
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(ctx.physical, &properties);
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical, &count, families.data());
    if (ctx.queueFamily >= count || !families[ctx.queueFamily].timestampValidBits
        || properties.limits.timestampPeriod <= 0)
    {
        brls::Logger::warning("Renderer: GPU timestamps unavailable; video draw time not logged");
        return;
    }
    timestampNs = properties.limits.timestampPeriod;
    VkQueryPoolCreateInfo info { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = 8; // per frame slot: video draw, EASU
    VkCommandBufferAllocateInfo alloc { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    alloc.commandPool        = ctx.commandPool;
    alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = resetCommands.size();
    if (vkCreateQueryPool(ctx.device, &info, nullptr, &timestamps) != VK_SUCCESS
        || vkAllocateCommandBuffers(ctx.device, &alloc, resetCommands.data()) != VK_SUCCESS)
    {
        brls::Logger::warning("Renderer: GPU timestamp setup failed; video draw time not logged");
        if (timestamps)
            vkDestroyQueryPool(ctx.device, timestamps, nullptr);
        timestamps = VK_NULL_HANDLE;
        resetCommands = {};
    }
}

void Renderer::collectTiming(brls::Switch2VulkanContext& ctx)
{
    // Borealis waited for this slot's fence before opening the frame, so the
    // timestamps written the last time it was used are complete.
    const uint32_t slot = ctx.frameIndex;
    auto elapsedMs = [&](uint32_t first, double& ms) {
        uint64_t ticks[2] {};
        if (vkGetQueryPoolResults(ctx.device, timestamps, first, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || ticks[1] < ticks[0])
            return false;
        ms = double(ticks[1] - ticks[0]) * timestampNs / 1e6;
        return true;
    };
    double ms = 0;
    if (timed[slot] && elapsedMs(slot * 4, ms))
    {
        drawSumMs += ms;
        drawMaxMs = std::max(drawMaxMs, ms);
        ++timedDraws;
    }
    if (easuTimed[slot] && elapsedMs(slot * 4 + 2, ms))
    {
        easuSumMs += ms;
        easuMaxMs = std::max(easuMaxMs, ms);
        ++easuRuns;
    }
    timed[slot] = easuTimed[slot] = false;
    const uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!timingStartUs)
        timingStartUs = now;
    else if (now - timingStartUs >= 5000000)
    {
        brls::Logger::info("Renderer: GPU last 5 s: video draw {} avg/max {:.2f}/{:.2f} ms; EASU {} avg/max "
                           "{:.2f}/{:.2f} ms; upscaling {}",
            timedDraws, timedDraws ? drawSumMs / timedDraws : 0.0, drawMaxMs, easuRuns,
            easuRuns ? easuSumMs / easuRuns : 0.0, easuMaxMs, lastEasu == 1 ? "EASU" : "bilinear");
        timingStartUs = now;
        timedDraws = easuRuns = 0;
        drawSumMs = drawMaxMs = easuSumMs = easuMaxMs = 0;
    }
    // Queries can't be reset inside Borealis's render pass: reset them in a
    // submit of their own, ahead of the frame on the same queue.
    VkCommandBuffer cmd = resetCommands[slot];
    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS || vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
        return;
    vkCmdResetQueryPool(cmd, timestamps, slot * 4, 4);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        return;
    VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    std::lock_guard lock(ctx.queueMutex);
    timed[slot] = vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
}

Renderer::~Renderer()
{
    auto* ctx = brls::Switch2VulkanContext::current();
    if (!ctx)
        return;
    std::lock_guard<std::mutex> lock(ctx->queueMutex);
    vkDeviceWaitIdle(ctx->device);
    for (auto& field : motion)
        ctx->destroyBuffer(field);
    for (auto& entry : upscaledCache)
        ctx->destroyBuffer(entry.buffer);
    ctx->destroyBuffer(placeholder);
    if (easuPipeline)
        vkDestroyPipeline(ctx->device, easuPipeline, nullptr);
    if (resetCommands[0])
        vkFreeCommandBuffers(ctx->device, ctx->commandPool, resetCommands.size(), resetCommands.data());
    if (timestamps)
        vkDestroyQueryPool(ctx->device, timestamps, nullptr);
    if (computeCommands[0])
        vkFreeCommandBuffers(ctx->device, ctx->commandPool, computeCommands.size(), computeCommands.data());
    if (flowPipeline)
        vkDestroyPipeline(ctx->device, flowPipeline, nullptr);
    if (predictPipeline)
        vkDestroyPipeline(ctx->device, predictPipeline, nullptr);
    for (auto& upload : uploads)
        ctx->destroyBuffer(upload);
    if (texelPipeline)
        vkDestroyPipeline(ctx->device, texelPipeline, nullptr);
    if (pipeline)
        vkDestroyPipeline(ctx->device, pipeline, nullptr);
    if (layout)
        vkDestroyPipelineLayout(ctx->device, layout, nullptr);
    if (pool)
        vkDestroyDescriptorPool(ctx->device, pool, nullptr);
    if (descriptors)
        vkDestroyDescriptorSetLayout(ctx->device, descriptors, nullptr);
}

bool Renderer::draw(const FramePtr& frame, const FramePtr& previous, bool reuseFlow)
{
    auto* context = brls::Switch2VulkanContext::current();
    if (!context || !frame || !frame->pool)
        return false;
    auto& ctx = *context;
    if (!pipeline)
        initialize();
    // First, so its query reset is submitted ahead of this frame's compute.
    if (timestamps)
        collectTiming(ctx);
    Params params {};
    params.extra[2]   = frame->width;
    params.extra[3]   = frame->height;
    const auto& image = frame->pool->decoder.images[frame->frame.slot];
    const Switch2ImportedImage imported = switch2FindImportedImage(image.backing);
    VkBuffer buffer       = imported.buffer;
    VkDeviceSize bytes    = imported.size;
    VkBufferView lumaView = imported.luma, chromaView = imported.chroma;
    if (buffer)
    {
        const auto& desc = image.descriptor;
        for (unsigned i = 0; i < 2; ++i)
        {
            const uint64_t rows = frame->height >> i;
            if (!rows || desc.planes[i].pitch < frame->width
                || uint64_t(desc.planes[i].offset) + desc.planes[i].span > imported.size
                || (rows - 1) * desc.planes[i].pitch + frame->width > desc.planes[i].span)
                throw std::runtime_error("Invalid native NV12 import geometry");
        }
        params.plane[0] = desc.planes[0].offset;
        params.plane[1] = desc.planes[0].pitch;
        params.plane[2] = desc.planes[1].offset;
        params.plane[3] = desc.planes[1].pitch;
        // Hold the frame (its decoder slot) until this command buffer completes.
        ctx.retain(frame);
    }
    else
    {
        // The import was refused: copy the planes into an upload buffer.
        const uint32_t yBytes = frame->width * frame->height;
        bytes                 = (VkDeviceSize(yBytes) + yBytes / 2 + 3) & ~VkDeviceSize(3);
        auto& upload          = uploads[ctx.frameIndex];
        if (upload.size < bytes)
        {
            ctx.destroyBuffer(upload);
            VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            if (ctx.texelBufferSampling)
                usage |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
            upload = ctx.createBuffer(bytes, usage);
        }
        uint32_t offset = 0;
        for (int i = 0; i < 2; ++i)
        {
            const uint32_t rows = i ? frame->height / 2 : frame->height;
            const uint8_t* plane = frame->frame.planes[i];
            const int pitch      = frame->frame.pitches[i];
            if (!plane || pitch < int(frame->width))
                throw std::runtime_error("Invalid decoded video plane");
            for (uint32_t row = 0; row < rows; ++row)
                std::memcpy(static_cast<uint8_t*>(upload.mapped) + offset + size_t(row) * frame->width,
                    plane + ptrdiff_t(row) * pitch, frame->width);
            params.plane[i * 2]     = offset;
            params.plane[i * 2 + 1] = frame->width;
            offset += frame->width * rows;
        }
        buffer     = upload.buffer;
        lumaView   = upload.luma;
        chromaView = upload.chroma;
    }
    // Letterbox to the display's aspect ratio.
    const float aspect = float(ctx.extent.width) * frame->height / (float(ctx.extent.height) * frame->width);
    params.region[2] = params.region[3] = 1;
    if (aspect > 1)
    {
        params.region[0] = 0.5f - 0.5f / aspect;
        params.region[2] = aspect;
    }
    else
    {
        params.region[1] = 0.5f - 0.5f * aspect;
        params.region[3] = 1 / aspect;
    }
    // Limited-range Rec.709 (the decoder's color policy).
    params.red[0] = params.green[0] = params.blue[0] = 1.1644f;
    params.red[2]   = 1.7927f;
    params.green[1] = -0.2132f;
    params.green[2] = -0.5329f;
    params.blue[1]  = 2.1124f;
    params.red[3]   = 0; // NV12
    params.green[3] = 16.0f / 255;
    // FSR 1 EASU only when the picture is enlarged: at 1:1 it would just
    // cost time. The shown size is the screen's divided by the letterbox scale.
    const uint32_t shownWidth = uint32_t(ctx.extent.width / params.region[2] + 0.5f),
                   shownHeight = uint32_t(ctx.extent.height / params.region[3] + 0.5f);
    const bool easu = upscaling && easuPipeline && computeCommands[0] && shownWidth > frame->width * 1.01f;
    if (int(easu) != lastEasu)
    {
        lastEasu = easu;
        brls::Logger::info("Renderer: upscaling {} {}x{} to {}x{}", easu ? "EASU" : "bilinear", frame->width,
            frame->height, shownWidth, shownHeight);
    }

    // Both images were validated when drawn as real frames. Keep their leases
    // through the graphics fence, which also covers the preceding flow submit.
    Switch2ImportedImage older {};
    if (previous && predictPipeline && imported.buffer && previous->pool == frame->pool
        && previous->width == frame->width && previous->height == frame->height)
    {
        const auto& oldImage = previous->pool->decoder.images[previous->frame.slot];
        older = switch2FindImportedImage(oldImage.backing);
        params.extra[0] = oldImage.descriptor.planes[0].offset;
        params.extra[1] = oldImage.descriptor.planes[0].pitch;
        params.previousChroma[0] = oldImage.descriptor.planes[1].offset;
        params.previousChroma[1] = oldImage.descriptor.planes[1].pitch;
    }
    const bool computeFlow = older.buffer != VK_NULL_HANDLE;
    const bool holdFlow    = !computeFlow && reuseFlow && mode == FrameGeneration::Extrapolate
        && predictPipeline && imported.buffer && flowFrame == frame.get()
        && flowWidth == frame->width && flowHeight == frame->height
        && flowSize > 0 && motion[flowSlot].buffer != VK_NULL_HANDLE;
    const bool predict = computeFlow || holdFlow;
    // EASU runs once per picture, on its first draw; later draws (repeats,
    // generated frames) sample the stored result. A midpoint needs both
    // pictures upscaled, or neither (only right after upscaling turns on).
    const bool midpoint = computeFlow && mode == FrameGeneration::Interpolate;
    Upscaled *upscaledFrame = nullptr, *upscaledEarlier = nullptr;
    bool runEasu = false;
    if (easu)
    {
        if (midpoint)
            upscaledEarlier = findUpscaled(previous, shownWidth, shownHeight);
        upscaledFrame = findUpscaled(frame, shownWidth, shownHeight);
        if (!upscaledFrame)
        {
            upscaledFrame = claimUpscaled(ctx, frame, shownWidth, shownHeight, upscaledEarlier);
            runEasu = true;
        }
        upscaledFrame->used = ++upscaledUses;
        if (upscaledEarlier)
            upscaledEarlier->used = ++upscaledUses;
    }
    const bool sampleUpscaled = upscaledFrame && (!midpoint || upscaledEarlier);
    params.blue[3] = sampleUpscaled ? 1.0f : 0.0f;
    // Without robust buffer access an out-of-bounds read faults the GPU (device
    // lost), and the compiler may run the upscaled reads even when blue.w turns
    // them off. Unused, the size is 1x1, so they clamp to the first word of
    // whatever is bound (the placeholder is 16 bytes).
    params.upscaledSize[0] = sampleUpscaled ? shownWidth : 1;
    params.upscaledSize[1] = sampleUpscaled ? shownHeight : 1;
    VkDescriptorBufferInfo descriptor { buffer, 0, bytes };
    VkWriteDescriptorSet writes[7] { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    writes[0].dstSet          = sets[ctx.frameIndex];
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo     = &descriptor;
    const bool texel = !predict && texelPipeline && lumaView && chromaView && !((params.plane[2] | params.plane[3]) & 1);
    if (texel)
    {
        for (unsigned i = 1; i < 3; ++i)
        {
            writes[i].dstSet          = sets[ctx.frameIndex];
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        }
        writes[1].pTexelBufferView = &lumaView;
        writes[2].pTexelBufferView = &chromaView;
    }
    const VkShaderStageFlags stagesUsed = predictionEnabled || upscaling
        ? VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    uint32_t writeCount = texel ? 3 : 1;
    VkDescriptorBufferInfo fieldInfo {}, olderInfo { older.buffer, 0, older.size };
    const uint32_t columns = (frame->width + 15) / 16, rows = (frame->height + 15) / 16;
    if (computeFlow)
    {
        ctx.retain(previous);
        auto& field = motion[ctx.frameIndex];
        VkDeviceSize size = columns * rows * 2 * sizeof(float);
        if (field.size < size)
        {
            ctx.destroyBuffer(field);
            field = ctx.createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }
        fieldInfo  = { field.buffer, 0, size };
        flowFrame  = frame.get();
        flowSlot   = ctx.frameIndex;
        flowSize   = size;
        flowWidth  = frame->width;
        flowHeight = frame->height;
        for (unsigned binding : { 3u, 4u })
        {
            auto& write = writes[writeCount++];
            write = writes[0];
            write.dstBinding = binding;
            write.pBufferInfo = binding == 3 ? &fieldInfo : &olderInfo;
        }
    }
    else if (holdFlow)
    {
        fieldInfo   = { motion[flowSlot].buffer, 0, flowSize };
        auto& write = writes[writeCount++];
        write = writes[0];
        write.dstBinding  = 3;
        write.pBufferInfo = &fieldInfo;
    }
    else
        flowFrame = nullptr;
    // Always bound (the shaders declare them); the placeholder when unused.
    VkDescriptorBufferInfo upscaledInfo { upscaledFrame ? upscaledFrame->buffer.buffer : placeholder.buffer, 0,
        VK_WHOLE_SIZE },
        earlierInfo { upscaledEarlier ? upscaledEarlier->buffer.buffer : placeholder.buffer, 0, VK_WHOLE_SIZE };
    for (unsigned binding : { 5u, 6u })
    {
        auto& write       = writes[writeCount++];
        write             = writes[0];
        write.dstBinding  = binding;
        write.pBufferInfo = binding == 5 ? &upscaledInfo : &earlierInfo;
    }
    vkUpdateDescriptorSets(ctx.device, writeCount, writes, 0, nullptr);
    if (computeFlow || runEasu)
    {
        const uint32_t slot = ctx.frameIndex;
        easuTimed[slot]     = runEasu && timestamps && timed[slot];
        // EASU writes the full shown size even when this draw doesn't sample it.
        Params compute          = params;
        compute.upscaledSize[0] = shownWidth;
        compute.upscaledSize[1] = shownHeight;
        submitCompute(ctx, computeCommands[slot], layout, sets[slot], compute, stagesUsed,
            runEasu ? Dispatch { easuPipeline, (shownWidth + 3) / 4, shownHeight } : Dispatch {},
            computeFlow ? Dispatch { flowPipeline, columns, rows } : Dispatch {},
            easuTimed[slot] ? timestamps : VK_NULL_HANDLE, slot * 4 + 2);
    }
    ctx.videoSource(imported.buffer != VK_NULL_HANDLE, texel);
    const int source = (imported.buffer ? 2 : 0) | int(texel);
    if (!predict && source != lastSource)
    {
        lastSource = source;
        brls::Logger::info("Renderer: source={} sampling={} {}x{}", imported.buffer ? "decoder buffer (no copy)" : "CPU copy",
            texel ? "texel buffer" : "storage buffer", frame->width, frame->height);
    }
    VkViewport viewport { 0, 0, float(ctx.extent.width), float(ctx.extent.height), 0, 1 };
    VkRect2D scissor { { 0, 0 }, ctx.extent };
    vkCmdSetViewport(ctx.command(), 0, 1, &viewport);
    vkCmdSetScissor(ctx.command(), 0, 1, &scissor);
    vkCmdBindPipeline(ctx.command(), VK_PIPELINE_BIND_POINT_GRAPHICS, predict ? predictPipeline : texel ? texelPipeline : pipeline);
    vkCmdBindDescriptorSets(ctx.command(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &sets[ctx.frameIndex], 0, nullptr);
    vkCmdPushConstants(ctx.command(), layout, stagesUsed, 0, sizeof(params), &params);
    const bool timing = timestamps && timed[ctx.frameIndex];
    if (timing)
        vkCmdWriteTimestamp(ctx.command(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, ctx.frameIndex * 4);
    vkCmdDraw(ctx.command(), 3, 1, 0, 0);
    if (timing)
        vkCmdWriteTimestamp(ctx.command(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, ctx.frameIndex * 4 + 1);
    return predict;
}

}
