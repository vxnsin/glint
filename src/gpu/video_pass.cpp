#include "gpu/video_pass.hpp"

#include <algorithm>

#include "core/log.hpp"
#include "gpu/context.hpp"
#include "shaders/fullscreen.vert.h"
#include "shaders/video.frag.h"

namespace glint::gpu {

namespace {

struct Push {
    float offset[2];
    float size[2];
    float color[4];
    float opacity;
    std::uint32_t tile;
    std::uint32_t encode;
    std::uint32_t bt709;
    std::uint32_t full_range;
    std::uint32_t interleaved;
    std::uint32_t pad[2];
};

VkShaderModule module(VkDevice device, const std::uint32_t* code, std::size_t bytes) {
    VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes,
        .pCode = code,
    };
    VkShaderModule result = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &info, nullptr, &result);
    return result;
}

VkSampler sampler(VkDevice device, VkSamplerAddressMode mode) {
    VkSamplerCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = mode,
        .addressModeV = mode,
        .addressModeW = mode,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    VkSampler result = VK_NULL_HANDLE;
    vkCreateSampler(device, &info, nullptr, &result);
    return result;
}

}  // namespace

VideoPass::VideoPass(Context& context) : context_(context) {
    VkDevice device = context_.device();
    VkDescriptorSetLayoutBinding bindings[] = {
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = 3,
        .pBindings = bindings,
    };
    vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout_);
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout_,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };
    vkCreatePipelineLayout(device, &layout_info, nullptr, &layout_);
    sampler_ = sampler(device, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    vertex_ = module(device, fullscreen_vert, sizeof(fullscreen_vert));
    fragment_ = module(device, video_frag, sizeof(video_frag));
}

VideoPass::~VideoPass() {
    VkDevice device = context_.device();
    for (auto& [format, pipeline] : pipelines_) vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyShaderModule(device, vertex_, nullptr);
    vkDestroyShaderModule(device, fragment_, nullptr);
    vkDestroySampler(device, sampler_, nullptr);
    vkDestroyPipelineLayout(device, layout_, nullptr);
    vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
}

VkPipeline VideoPass::pipeline(VkFormat format) {
    if (auto it = pipelines_.find(static_cast<int>(format)); it != pipelines_.end()) {
        return it->second;
    }
    VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex_,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragment_,
         .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    // Straight alpha: the picture's own alpha is resolved against the background colour in
    // the shader, the output alpha is the crossfade opacity.
    VkPipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };
    VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    VkGraphicsPipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = layout_,
    };
    VkPipeline result = VK_NULL_HANDLE;
    if (VkResult r = vkCreateGraphicsPipelines(context_.device(), context_.pipeline_cache(), 1,
                                               &info, nullptr, &result);
        r != VK_SUCCESS) {
        log::error("vkCreateGraphicsPipelines: {}", result_name(r));
        return VK_NULL_HANDLE;
    }
    pipelines_[static_cast<int>(format)] = result;
    context_.save_pipeline_cache();
    return result;
}

void VideoPass::draw(const Target& target, const Frame& frame, Scaling scaling,
                     const float color[3], float opacity) {
    VkPipeline pipe = pipeline(target.format);
    if (pipe == VK_NULL_HANDLE) return;

    auto tw = static_cast<float>(target.extent.width);
    auto th = static_cast<float>(target.extent.height);
    auto iw = static_cast<float>(frame.width);
    auto ih = static_cast<float>(frame.height);
    float k = 1.0f;
    float w = iw, h = ih;
    switch (scaling) {
        case Scaling::fill: k = std::max(tw / iw, th / ih); break;
        case Scaling::fit: k = std::min(tw / iw, th / ih); break;
        case Scaling::center:
        case Scaling::tile: k = 1.0f; break;
        case Scaling::stretch: break;
    }
    if (scaling == Scaling::stretch) {
        w = tw;
        h = th;
    } else {
        w = iw * k;
        h = ih * k;
    }
    Push push{};
    if (scaling == Scaling::tile) {
        push.offset[0] = push.offset[1] = 0;
    } else {
        push.offset[0] = (tw - w) * 0.5f;
        push.offset[1] = (th - h) * 0.5f;
    }
    push.size[0] = w;
    push.size[1] = h;
    push.color[0] = color[0];
    push.color[1] = color[1];
    push.color[2] = color[2];
    push.color[3] = 1.0f;
    push.opacity = opacity;
    push.tile = scaling == Scaling::tile ? 1u : 0u;
    push.encode = target.encode_srgb ? 1u : 0u;
    push.bt709 = frame.bt709 ? 1u : 0u;
    push.full_range = frame.full_range ? 1u : 0u;
    push.interleaved = frame.interleaved ? 1u : 0u;

    VkDescriptorImageInfo planes[] = {
        {sampler_, frame.y, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler_, frame.u, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler_, frame.v != VK_NULL_HANDLE ? frame.v : frame.u,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet writes[3];
    for (std::uint32_t i = 0; i < 3; ++i) {
        writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstBinding = i + 1,
                     .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     .pImageInfo = &planes[i]};
    }
    VkCommandBuffer cmd = target.cmd;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 3, writes);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

}  // namespace glint::gpu
