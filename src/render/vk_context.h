#pragma once
// Vulkan setup via vk-bootstrap + VMA. Either presents to an SDL3 window (swapchain) or renders into an
// offscreen image that can be read back for --shot screenshots (no window, works on lavapipe).
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;

namespace wisp {

class VkContext {
public:
    bool init(SDL_Window* window, uint32_t w, uint32_t h, std::string& err);
    void shutdown();
    bool offscreen() const { return window_ == nullptr; }
    int framesInFlight() const { return framesInFlight_; }
    uint32_t imageCount() const { return uint32_t(framebuffers_.size()); }

    // Returns a command buffer inside an active render pass, or VK_NULL_HANDLE if the frame must be skipped.
    VkCommandBuffer beginFrame(int& slot);
    void endFrame();
    void requestResize(uint32_t w, uint32_t h) { pendingW_ = w; pendingH_ = h; needRecreate_ = true; }
    bool readback(std::vector<uint8_t>& rgba);  // offscreen only; RGBA8

    vkb::Instance vkbInstance;
    vkb::Device vkbDevice;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VmaAllocator vma = nullptr;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::string deviceName;

private:
    bool createTargets(std::string& err);
    void destroyTargets();
    bool recreateSwapchain();

    SDL_Window* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    vkb::Swapchain swapchain_;
    VkFormat format_ = VK_FORMAT_R8G8B8A8_UNORM;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> renderDone_;
    // offscreen target
    VkImage offImage_ = VK_NULL_HANDLE;
    VmaAllocation offAlloc_ = nullptr;
    VkBuffer readBuf_ = VK_NULL_HANDLE;
    VmaAllocation readAlloc_ = nullptr;

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquire = VK_NULL_HANDLE;
    };
    Frame frames_[2];
    int framesInFlight_ = 2;
    int slot_ = 0;
    uint32_t imageIndex_ = 0;
    bool needRecreate_ = false;
    uint32_t pendingW_ = 0, pendingH_ = 0;
};

}  // namespace wisp
