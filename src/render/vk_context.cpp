#define VMA_IMPLEMENTATION
#include "render/vk_context.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstring>

namespace wisp {

namespace {
constexpr float kClear[4] = {0x0F / 255.f, 0x11 / 255.f, 0x15 / 255.f, 1.f};
}

bool VkContext::init(SDL_Window* window, uint32_t w, uint32_t h, std::string& err) {
    window_ = window;
    extent = {w, h};
    framesInFlight_ = window ? 2 : 1;

    vkb::InstanceBuilder ib;
    ib.set_app_name("ProjectWisp").require_api_version(1, 1, 0);
    if (!window) {
        ib.set_headless(true);
    } else {
        Uint32 n = 0;
        const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&n);
        for (Uint32 i = 0; i < n; ++i) ib.enable_extension(exts[i]);
    }
    // vk-bootstrap enables VK_KHR_portability_enumeration automatically when present (MoltenVK).
    auto ir = ib.build();
    if (!ir) { err = "vkb instance: " + ir.error().message(); return false; }
    vkbInstance = ir.value();

    if (window && !SDL_Vulkan_CreateSurface(window, vkbInstance.instance, nullptr, &surface_)) {
        err = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
        return false;
    }
    vkb::PhysicalDeviceSelector sel(vkbInstance);
    sel.set_minimum_version(1, 1).prefer_gpu_device_type(vkb::PreferredDeviceType::discrete).allow_any_gpu_device_type(true);
    if (window) sel.set_surface(surface_);
    auto pr = sel.select();
    if (!pr) { err = "vkb physical device: " + pr.error().message(); return false; }
    physical = pr.value().physical_device;
    deviceName = pr.value().name;

    vkb::DeviceBuilder db(pr.value());
    auto dr = db.build();
    if (!dr) { err = "vkb device: " + dr.error().message(); return false; }
    vkbDevice = dr.value();
    device = vkbDevice.device;
    queue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    queueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

    VmaAllocatorCreateInfo ai{};
    ai.physicalDevice = physical;
    ai.device = device;
    ai.instance = vkbInstance.instance;
    ai.vulkanApiVersion = VK_API_VERSION_1_1;
    if (vmaCreateAllocator(&ai, &vma) != VK_SUCCESS) { err = "vmaCreateAllocator failed"; return false; }

    if (window) {
        vkb::SwapchainBuilder sb(vkbDevice, surface_);
        auto sr = sb.set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                      .add_fallback_format({VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                      .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                      .set_desired_extent(w, h)
                      .build();
        if (!sr) { err = "swapchain: " + sr.error().message(); return false; }
        swapchain_ = sr.value();
        format_ = swapchain_.image_format;
        extent = swapchain_.extent;
    }

    VkAttachmentDescription att{};
    att.format = format_;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = window ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    if (vkCreateRenderPass(device, &rpi, nullptr, &renderPass) != VK_SUCCESS) { err = "render pass"; return false; }

    for (int i = 0; i < framesInFlight_; ++i) {
        Frame& f = frames_[i];
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily;
        vkCreateCommandPool(device, &pci, nullptr, &f.pool);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = f.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &cai, &f.cmd);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device, &fci, nullptr, &f.fence);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device, &sci, nullptr, &f.acquire);
    }
    return createTargets(err);
}

bool VkContext::createTargets(std::string& err) {
    if (window_) {
        auto imgs = swapchain_.get_images().value();
        views_ = swapchain_.get_image_views().value();
        (void)imgs;
    } else {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format_;
        ici.extent = {extent.width, extent.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(vma, &ici, &aci, &offImage_, &offAlloc_, nullptr) != VK_SUCCESS) { err = "offscreen image"; return false; }
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = offImage_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        vkCreateImageView(device, &vci, nullptr, &v);
        views_ = {v};
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = VkDeviceSize(extent.width) * extent.height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo bai{};
        bai.usage = VMA_MEMORY_USAGE_AUTO;
        bai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(vma, &bci, &bai, &readBuf_, &readAlloc_, nullptr) != VK_SUCCESS) { err = "readback buffer"; return false; }
    }
    for (VkImageView v : views_) {
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = renderPass;
        fci.attachmentCount = 1;
        fci.pAttachments = &v;
        fci.width = extent.width;
        fci.height = extent.height;
        fci.layers = 1;
        VkFramebuffer fb;
        vkCreateFramebuffer(device, &fci, nullptr, &fb);
        framebuffers_.push_back(fb);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s;
        vkCreateSemaphore(device, &sci, nullptr, &s);
        renderDone_.push_back(s);
    }
    return true;
}

void VkContext::destroyTargets() {
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device, fb, nullptr);
    for (auto s : renderDone_) vkDestroySemaphore(device, s, nullptr);
    framebuffers_.clear();
    renderDone_.clear();
    if (window_) {
        swapchain_.destroy_image_views(views_);
    } else {
        for (auto v : views_) vkDestroyImageView(device, v, nullptr);
        if (offImage_) vmaDestroyImage(vma, offImage_, offAlloc_);
        if (readBuf_) vmaDestroyBuffer(vma, readBuf_, readAlloc_);
        offImage_ = VK_NULL_HANDLE;
        readBuf_ = VK_NULL_HANDLE;
    }
    views_.clear();
}

bool VkContext::recreateSwapchain() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (w == 0 || h == 0) return false;  // minimised
    vkDeviceWaitIdle(device);
    destroyTargets();
    vkb::SwapchainBuilder sb(vkbDevice, surface_);
    auto sr = sb.set_desired_format({format_, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                  .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                  .set_desired_extent(uint32_t(w), uint32_t(h))
                  .set_old_swapchain(swapchain_)
                  .build();
    if (!sr) return false;
    vkb::destroy_swapchain(swapchain_);
    swapchain_ = sr.value();
    extent = swapchain_.extent;
    std::string err;
    createTargets(err);
    needRecreate_ = false;
    return true;
}

VkCommandBuffer VkContext::beginFrame(int& slot) {
    if (window_ && needRecreate_ && !recreateSwapchain()) return VK_NULL_HANDLE;
    Frame& f = frames_[slot_];
    vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    if (window_) {
        VkResult r = vkAcquireNextImageKHR(device, swapchain_.swapchain, UINT64_MAX, f.acquire, VK_NULL_HANDLE, &imageIndex_);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { needRecreate_ = true; return VK_NULL_HANDLE; }
        if (r == VK_SUBOPTIMAL_KHR) needRecreate_ = true;
    } else {
        imageIndex_ = 0;
    }
    vkResetFences(device, 1, &f.fence);
    vkResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    VkClearValue clear{};
    std::memcpy(clear.color.float32, kClear, sizeof(kClear));
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = renderPass;
    rbi.framebuffer = framebuffers_[imageIndex_];
    rbi.renderArea = {{0, 0}, extent};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(f.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    slot = slot_;
    return f.cmd;
}

void VkContext::endFrame() {
    Frame& f = frames_[slot_];
    vkCmdEndRenderPass(f.cmd);
    if (!window_) {
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(f.cmd, offImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readBuf_, 1, &c);
    }
    vkEndCommandBuffer(f.cmd);
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    if (window_) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquire;
        si.pWaitDstStageMask = &waitStage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderDone_[imageIndex_];
    }
    vkQueueSubmit(queue, 1, &si, f.fence);
    if (window_) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderDone_[imageIndex_];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_.swapchain;
        pi.pImageIndices = &imageIndex_;
        VkResult r = vkQueuePresentKHR(queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) needRecreate_ = true;
    }
    slot_ = (slot_ + 1) % framesInFlight_;
}

bool VkContext::readback(std::vector<uint8_t>& rgba) {
    if (window_) return false;
    vkDeviceWaitIdle(device);
    vmaInvalidateAllocation(vma, readAlloc_, 0, VK_WHOLE_SIZE);
    VmaAllocationInfo info;
    vmaGetAllocationInfo(vma, readAlloc_, &info);
    size_t n = size_t(extent.width) * extent.height * 4;
    rgba.resize(n);
    std::memcpy(rgba.data(), info.pMappedData, n);
    for (size_t i = 3; i < n; i += 4) rgba[i] = 255;
    return true;
}

void VkContext::shutdown() {
    if (!device) return;
    vkDeviceWaitIdle(device);
    destroyTargets();
    for (int i = 0; i < framesInFlight_; ++i) {
        vkDestroyFence(device, frames_[i].fence, nullptr);
        vkDestroySemaphore(device, frames_[i].acquire, nullptr);
        vkDestroyCommandPool(device, frames_[i].pool, nullptr);
    }
    vkDestroyRenderPass(device, renderPass, nullptr);
    if (window_) vkb::destroy_swapchain(swapchain_);
    vmaDestroyAllocator(vma);
    vkb::destroy_device(vkbDevice);
    if (surface_) vkb::destroy_surface(vkbInstance, surface_);
    vkb::destroy_instance(vkbInstance);
    device = VK_NULL_HANDLE;
}

}  // namespace wisp
