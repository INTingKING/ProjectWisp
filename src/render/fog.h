#pragma once
// Fog pass: custom Vulkan pipeline invoked from inside the ImGui draw stream via ImDrawList callbacks,
// so fog is layered exactly where the UI code asks for it (above tracks, below labels/hatching).
#include <imgui.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <vector>

namespace wisp {

struct FogRect {
    float x0, y0, x1, y1;
    float alpha;      // 0.55 for nothing-known, 0.40 while loading
    float edgeX = 24.f, edgeY = 24.f;  // soft edge widths in px
};

class FogRenderer {
public:
    bool init(VkDevice device, VmaAllocator vma, VkRenderPass rp, int frames);
    void shutdown();
    void beginFrame(float time, ImVec2 drift, ImVec2 viewport);
    void draw(ImDrawList* dl, const std::vector<FogRect>& rects);
    void upload(int slot);

private:
    struct Vtx { float px, py, rx0, ry0, rx1, ry1, alpha, ex, ey; };
    struct Batch { FogRenderer* self; uint32_t first, count; };
    struct Push { float vw, vh, time, pad0, dx, dy, pad1, pad2, tint[4]; };
    struct Buf { VkBuffer buf = VK_NULL_HANDLE; VmaAllocation alloc = nullptr; void* map = nullptr; size_t cap = 0; };
    static void callback(const ImDrawList* dl, const ImDrawCmd* cmd);

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator vma_ = nullptr;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    Buf bufs_[2];
    int slot_ = 0;
    std::vector<Vtx> verts_;
    Push push_{};
};

}  // namespace wisp
