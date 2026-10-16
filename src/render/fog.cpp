#include "render/fog.h"

#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cstring>

#include "fog.frag.h"
#include "fog.vert.h"

namespace wisp {

namespace {
VkShaderModule makeModule(VkDevice d, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(d, &ci, nullptr, &m);
    return m;
}
}  // namespace

bool FogRenderer::init(VkDevice device, VmaAllocator vma, VkRenderPass rp, int frames) {
    device_ = device;
    vma_ = vma;
    (void)frames;
    VkShaderModule vs = makeModule(device, fog_vert_spv, sizeof(fog_vert_spv));
    VkShaderModule fs = makeModule(device, fog_frag_spv, sizeof(fog_frag_spv));

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device, &lci, nullptr, &layout_) != VK_SUCCESS) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    VkVertexInputBindingDescription bind{0, sizeof(Vtx), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 24},
    };
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 3;
    vin.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vin;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dsi;
    pci.layout = layout_;
    pci.renderPass = rp;
    VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline_);
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);
    return r == VK_SUCCESS;
}

void FogRenderer::shutdown() {
    for (auto& b : bufs_)
        if (b.buf) vmaDestroyBuffer(vma_, b.buf, b.alloc);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
}

void FogRenderer::beginFrame(float time, ImVec2 drift, ImVec2 viewport, float fbScale) {
    verts_.clear();
    fbScale_ = fbScale;
    push_ = {};
    push_.vw = viewport.x;
    push_.vh = viewport.y;
    push_.time = time;
    push_.dx = drift.x;
    push_.dy = drift.y;
    push_.tint[0] = 0xE9 / 255.f;
    push_.tint[1] = 0xE4 / 255.f;
    push_.tint[2] = 0xD8 / 255.f;
    push_.tint[3] = 1.f;
}

void FogRenderer::draw(ImDrawList* dl, const std::vector<FogRect>& rects, float octave1) {
    if (rects.empty()) return;
    Batch b{this, uint32_t(verts_.size()), 0, octave1};
    for (const FogRect& r : rects) {
        if (r.x1 <= r.x0 || r.y1 <= r.y0 || r.alpha <= 0.f) continue;
        float ex = std::min(r.edgeX, (r.x1 - r.x0) * 0.5f), ey = std::min(r.edgeY, (r.y1 - r.y0) * 0.5f);
        auto v = [&](float x, float y) { verts_.push_back({x, y, r.x0, r.y0, r.x1, r.y1, r.alpha, ex, ey}); };
        v(r.x0, r.y0); v(r.x1, r.y0); v(r.x1, r.y1);
        v(r.x0, r.y0); v(r.x1, r.y1); v(r.x0, r.y1);
    }
    b.count = uint32_t(verts_.size()) - b.first;
    if (!b.count) return;
    dl->AddCallback(&FogRenderer::callback, &b, sizeof(b));
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

void FogRenderer::upload(int slot) {
    slot_ = slot;
    Buf& b = bufs_[slot];
    size_t need = std::max<size_t>(verts_.size() * sizeof(Vtx), 1024);
    if (need > b.cap) {
        if (b.buf) vmaDestroyBuffer(vma_, b.buf, b.alloc);  // safe: this slot's fence has been waited on
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = need * 2;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info;
        vmaCreateBuffer(vma_, &bci, &aci, &b.buf, &b.alloc, &info);
        b.map = info.pMappedData;
        b.cap = need * 2;
    }
    if (!verts_.empty()) {
        std::memcpy(b.map, verts_.data(), verts_.size() * sizeof(Vtx));
        vmaFlushAllocation(vma_, b.alloc, 0, VK_WHOLE_SIZE);
    }
}

void FogRenderer::callback(const ImDrawList*, const ImDrawCmd* cmd) {
    const Batch* b = static_cast<const Batch*>(cmd->UserCallbackData);
    FogRenderer* self = b->self;
    auto* rs = static_cast<ImGui_ImplVulkan_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    VkCommandBuffer cb = rs->CommandBuffer;
    const Buf& buf = self->bufs_[self->slot_];
    if (!buf.buf) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, self->pipeline_);
    const float k = self->fbScale_;  // geometry stays in ImGui units (shader divides by push_.vw/vh); viewport + scissor in pixels
    VkViewport vp{0, 0, self->push_.vw * k, self->push_.vh * k, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    float x0 = std::max(0.f, cmd->ClipRect.x * k), y0 = std::max(0.f, cmd->ClipRect.y * k);
    float x1 = std::min(self->push_.vw * k, cmd->ClipRect.z * k), y1 = std::min(self->push_.vh * k, cmd->ClipRect.w * k);
    if (x1 <= x0 || y1 <= y0) return;
    VkRect2D sc{{int32_t(x0), int32_t(y0)}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &buf.buf, &off);
    Push pc = self->push_;
    pc.octave1 = b->octave1;
    vkCmdPushConstants(cb, self->layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &pc);
    vkCmdDraw(cb, b->count, 1, b->first, 0);
}

}  // namespace wisp
