// Adapted from Moonlight-Switch (XITRIX, GPL-3.0): VulkanVideoRenderer.cpp.
// initialize() and the destructor are Moonlight's; draw() takes our frames.
#include "renderer.hpp"

#include "image_interop.hpp"
#include "video_spirv.hpp"

#include <borealis.hpp>
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
};
static_assert(sizeof(Params) == 104);
VkShaderModule shader(brls::Switch2VulkanContext& ctx, const uint32_t* code, size_t size)
{
    VkShaderModuleCreateInfo info { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = size;
    info.pCode    = code;
    VkShaderModule result;
    ctx.check(vkCreateShaderModule(ctx.device, &info, nullptr, &result), "video shader");
    return result;
}

void submitFlow(brls::Switch2VulkanContext& ctx, VkCommandBuffer cmd, VkPipelineLayout layout,
    VkPipeline flowPipeline, VkDescriptorSet set, const Params& params, VkShaderStageFlags stagesUsed,
    uint32_t columns, uint32_t rows)
{
    // Borealis has already opened the UI render pass. Submit compute ahead
    // of it on the same queue. A generated draw always requests a present.
    ctx.check(vkResetCommandBuffer(cmd, 0), "reset flow commands");
    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ctx.check(vkBeginCommandBuffer(cmd, &begin), "begin flow");
    VkMemoryBarrier barrier { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, flowPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout, stagesUsed, 0, sizeof(params), &params);
    vkCmdDispatch(cmd, (columns + 7) / 8, (rows + 7) / 8, 1);
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
    ctx.check(vkEndCommandBuffer(cmd), "end flow");
    VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    std::lock_guard lock(ctx.queueMutex);
    ctx.check(vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE), "submit flow");
}

}

void Renderer::initialize()
{
    auto& ctx = *brls::Switch2VulkanContext::current();
    if (predictionEnabled)
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
        predictionEnabled = (families[ctx.queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        if (!predictionEnabled)
            brls::Logger::warning("Frame generation: graphics queue lacks compute support");
    }
    std::vector<VkDescriptorSetLayoutBinding> bindings {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
    };
    if (!ctx.texelBufferSampling)
        bindings.resize(1);
    VkShaderStageFlags stagesUsed = VK_SHADER_STAGE_FRAGMENT_BIT;
    if (predictionEnabled)
    {
        stagesUsed |= VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[0].stageFlags = stagesUsed;
        bindings.push_back({ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stagesUsed, nullptr });
        bindings.push_back({ 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stagesUsed, nullptr });
    }
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
    VkDescriptorPoolSize sizes[] { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, predictionEnabled ? 6u : 2u },
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
        if (result == VK_SUCCESS)
        {
            VkCommandBufferAllocateInfo alloc { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            alloc.commandPool = ctx.commandPool;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = flowCommands.size();
            ctx.check(vkAllocateCommandBuffers(ctx.device, &alloc, flowCommands.data()), "flow commands");
        }
        else
            brls::Logger::warning("Frame generation: pipeline creation failed rc={}", int(result));
    }
    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
    ctx.check(rc, "video pipeline");
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
    if (flowCommands[0])
        vkFreeCommandBuffers(ctx->device, ctx->commandPool, flowCommands.size(), flowCommands.data());
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

bool Renderer::draw(const FramePtr& frame, const FramePtr& previous)
{
    auto* context = brls::Switch2VulkanContext::current();
    if (!context || !frame || !frame->pool)
        return false;
    auto& ctx = *context;
    if (!pipeline)
        initialize();
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
    const bool predict = older.buffer != VK_NULL_HANDLE;
    VkDescriptorBufferInfo descriptor { buffer, 0, bytes };
    VkWriteDescriptorSet writes[5] { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
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
    const VkShaderStageFlags stagesUsed = predictionEnabled
        ? VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    uint32_t writeCount = texel ? 3 : 1;
    VkDescriptorBufferInfo fieldInfo {}, olderInfo { older.buffer, 0, older.size };
    const uint32_t columns = (frame->width + 15) / 16, rows = (frame->height + 15) / 16;
    if (predict)
    {
        ctx.retain(previous);
        auto& field = motion[ctx.frameIndex];
        VkDeviceSize size = columns * rows * 2 * sizeof(float);
        if (field.size < size)
        {
            ctx.destroyBuffer(field);
            field = ctx.createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }
        fieldInfo = { field.buffer, 0, size };
        for (unsigned binding : { 3u, 4u })
        {
            auto& write = writes[writeCount++];
            write = writes[0];
            write.dstBinding = binding;
            write.pBufferInfo = binding == 3 ? &fieldInfo : &olderInfo;
        }
    }
    vkUpdateDescriptorSets(ctx.device, writeCount, writes, 0, nullptr);
    if (predict)
    {
        submitFlow(ctx, flowCommands[ctx.frameIndex], layout, flowPipeline, sets[ctx.frameIndex],
            params, stagesUsed, columns, rows);
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
    vkCmdDraw(ctx.command(), 3, 1, 0, 0);
    return predict;
}

}
