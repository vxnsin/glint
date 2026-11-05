#include "gpu/blit.hpp"

#include "core/log.hpp"
#include "gpu/context.hpp"
#include "shaders/fullscreen.vert.h"
#include "shaders/texture.frag.h"

namespace glint::gpu {

namespace {

struct Push {
    float size[2];
    float uv[2];
    float opacity;
    std::uint32_t decode;
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

}  // namespace

Blit::Blit(Context& context) : context_(context) {
    VkDevice device = context_.device();
    VkDescriptorSetLayoutBinding binding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = 1,
        .pBindings = &binding,
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
    vertex_ = module(device, fullscreen_vert, sizeof(fullscreen_vert));
    fragment_ = module(device, texture_frag, sizeof(texture_frag));
    VkSamplerCreateInfo sampler{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    vkCreateSampler(device, &sampler, nullptr, &linear_);
}

Blit::~Blit() {
    VkDevice device = context_.device();
    for (auto& [key, p] : pipelines_) vkDestroyPipeline(device, p, nullptr);
    vkDestroySampler(device, linear_, nullptr);
    vkDestroyShaderModule(device, vertex_, nullptr);
    vkDestroyShaderModule(device, fragment_, nullptr);
    vkDestroyPipelineLayout(device, layout_, nullptr);
    vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
}

VkPipeline Blit::pipeline(VkFormat format, bool blend) {
    int key = static_cast<int>(format) * 2 + (blend ? 1 : 0);
    if (auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;
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
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState attachment{
        .blendEnable = blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
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
        .pColorBlendState = &blend_info,
        .pDynamicState = &dynamic,
        .layout = layout_,
    };
    VkPipeline result = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(context_.device(), context_.pipeline_cache(), 1, &info, nullptr,
                                  &result) != VK_SUCCESS) {
        log::error("blit pipeline failed");
        result = VK_NULL_HANDLE;
    }
    pipelines_[key] = result;
    return result;
}

void Blit::draw(VkCommandBuffer cmd, VkFormat format, VkImageView source, VkSampler sampler,
                const Params& params) {
    VkPipeline p = pipeline(format, params.blend);
    if (p == VK_NULL_HANDLE) return;
    Push push{{params.width, params.height},
              {params.u, params.v},
              params.opacity,
              params.decode ? 1u : 0u};
    VkDescriptorImageInfo image{sampler, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstBinding = 1,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &write);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

}  // namespace glint::gpu
