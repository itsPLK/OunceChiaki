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
};
static_assert(sizeof(Params) == 96);
VkShaderModule shader(brls::Switch2VulkanContext& ctx, const uint32_t* code, size_t size)
{
    VkShaderModuleCreateInfo info { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = size;
    info.pCode    = code;
    VkShaderModule result;
    ctx.check(vkCreateShaderModule(ctx.device, &info, nullptr, &result), "video shader");
    return result;
}

}

void Renderer::initialize()
{
    auto& ctx = *brls::Switch2VulkanContext::current();
    VkDescriptorSetLayoutBinding bindings[] {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
    };
    VkDescriptorSetLayoutCreateInfo descriptorInfo { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    descriptorInfo.bindingCount = ctx.texelBufferSampling ? 3 : 1;
    descriptorInfo.pBindings    = bindings;
    ctx.check(vkCreateDescriptorSetLayout(ctx.device, &descriptorInfo, nullptr, &descriptors), "video descriptors");
    VkPushConstantRange constants { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Params) };
    VkPipelineLayoutCreateInfo layoutInfo { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &descriptors;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &constants;
    ctx.check(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &layout), "video layout");
    VkDescriptorPoolSize sizes[] { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 },
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

void Renderer::draw(const FramePtr& frame)
{
    auto* context = brls::Switch2VulkanContext::current();
    if (!context || !frame || !frame->pool)
        return;
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

    VkDescriptorBufferInfo descriptor { buffer, 0, bytes };
    VkWriteDescriptorSet writes[3] { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    writes[0].dstSet          = sets[ctx.frameIndex];
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo     = &descriptor;
    const bool texel = texelPipeline && lumaView && chromaView && !((params.plane[2] | params.plane[3]) & 1);
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
    vkUpdateDescriptorSets(ctx.device, texel ? 3 : 1, writes, 0, nullptr);
    ctx.videoSource(imported.buffer != VK_NULL_HANDLE, texel);
    const int source = (imported.buffer ? 2 : 0) | int(texel);
    if (source != lastSource)
    {
        lastSource = source;
        brls::Logger::info("Renderer: source={} sampling={} {}x{}", imported.buffer ? "decoder buffer (no copy)" : "CPU copy",
            texel ? "texel buffer" : "storage buffer", frame->width, frame->height);
    }
    VkViewport viewport { 0, 0, float(ctx.extent.width), float(ctx.extent.height), 0, 1 };
    VkRect2D scissor { { 0, 0 }, ctx.extent };
    vkCmdSetViewport(ctx.command(), 0, 1, &viewport);
    vkCmdSetScissor(ctx.command(), 0, 1, &scissor);
    vkCmdBindPipeline(ctx.command(), VK_PIPELINE_BIND_POINT_GRAPHICS, texel ? texelPipeline : pipeline);
    vkCmdBindDescriptorSets(ctx.command(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &sets[ctx.frameIndex], 0, nullptr);
    vkCmdPushConstants(ctx.command(), layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);
    vkCmdDraw(ctx.command(), 3, 1, 0, 0);
}

}
