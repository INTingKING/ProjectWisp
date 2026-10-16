// ProjectWisp native explorer: SDL3 + Dear ImGui + Vulkan.
//   wisp [--data DIR] [--region chr22:1-50000] [--pin chr22:12345] [--reduced-motion]
//   wisp --shot out.png [--size 1600x1000] [--time 3.0] ...   (offscreen render, no window)
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "render/fog.h"
#include "render/png_write.h"
#include "render/vk_context.h"
#include "ui/app.h"
#include "ui/theme.h"

using namespace wisp;

static std::string findAssets(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe = fs::weakly_canonical(fs::path(argv0), ec).parent_path();
    for (fs::path p : {fs::path("assets"), exe / "assets", exe / "../assets", fs::path(WISP_ASSET_DIR)})
        if (fs::exists(p / "fonts", ec)) return p.string();
    return WISP_ASSET_DIR;
}

int main(int argc, char** argv) {
    AppOptions opt;
    std::string shot;
    int W = 1600, H = 1000;
    float shotTime = 2.0f;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--data") opt.dataDir = next();
        else if (a == "--region") opt.region = next();
        else if (a == "--pin") opt.pin = next();
        else if (a == "--reduced-motion") opt.reducedMotion = true;
        else if (a == "--shot") shot = next();
        else if (a == "--time") shotTime = std::strtof(next().c_str(), nullptr);
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &W, &H);
        else if (a == "-h" || a == "--help") {
            std::printf("usage: wisp [--data DIR] [--region LOCUS] [--pin LOCUS] [--reduced-motion]\n"
                        "            [--shot out.png --size WxH --time SECONDS]\n");
            return 0;
        }
    }
    const bool offscreen = !shot.empty();

    SDL_Window* window = nullptr;
    if (!offscreen) {
        if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
        window = SDL_CreateWindow("ProjectWisp", W, H, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!window) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    }
    VkContext vk;
    std::string err;
    if (!vk.init(window, uint32_t(W), uint32_t(H), err)) { std::fprintf(stderr, "vulkan: %s\n", err.c_str()); return 1; }
    std::fprintf(stderr, "vulkan device: %s\n", vk.deviceName.c_str());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    theme::apply(findAssets(argv[0]).c_str());
    if (window) ImGui_ImplSDL3_InitForVulkan(window);
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_1;
    ii.Instance = vk.vkbInstance.instance;
    ii.PhysicalDevice = vk.physical;
    ii.Device = vk.device;
    ii.QueueFamily = vk.queueFamily;
    ii.Queue = vk.queue;
    ii.DescriptorPoolSize = 64;
    ii.RenderPass = vk.renderPass;
    ii.MinImageCount = 2;
    ii.ImageCount = std::max<uint32_t>(2, vk.imageCount());
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&ii);

    FogRenderer fog;
    if (!fog.init(vk.device, vk.vma, vk.renderPass, vk.framesInFlight())) { std::fprintf(stderr, "fog pipeline failed\n"); return 1; }

    int rc = 0;
    {
        App app;
        if (!app.init(opt, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            rc = 1;
        } else {
            auto t0 = std::chrono::steady_clock::now();
            bool running = true;
            int frame = 0, idleFrames = 0;
            while (running) {
                if (window) {
                    SDL_Event e;
                    while (SDL_PollEvent(&e)) {
                        ImGui_ImplSDL3_ProcessEvent(&e);
                        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
                        if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) vk.requestResize(uint32_t(e.window.data1), uint32_t(e.window.data2));
                    }
                    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) { SDL_Delay(16); continue; }
                }
                ImGui_ImplVulkan_NewFrame();
                if (window) {
                    ImGui_ImplSDL3_NewFrame();
                } else {
                    io.DisplaySize = ImVec2(float(W), float(H));
                    io.DeltaTime = 1.f / 60.f;
                    io.MousePos = ImVec2(-1, -1);
                }
                ImGui::NewFrame();
                float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
                float t = offscreen ? shotTime : secs;
                ImVec2 drift = app.reducedMotion() ? ImVec2(0, 0) : ImVec2(6.f, 2.f);  // px/s
                fog.beginFrame(t, drift, io.DisplaySize);
                app.frame(fog);
                ImGui::Render();
                int slot = 0;
                VkCommandBuffer cmd = vk.beginFrame(slot);
                if (!cmd) continue;
                fog.upload(slot);
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
                vk.endFrame();
                if (offscreen) {
                    ++frame;
                    idleFrames = app.idle() ? idleFrames + 1 : 0;
                    if ((frame >= 4 && idleFrames >= 3) || secs > 90.f) {
                        std::vector<uint8_t> px;
                        vk.readback(px);
                        if (!writePng(shot, int(vk.extent.width), int(vk.extent.height), px)) { std::fprintf(stderr, "png write failed\n"); rc = 1; }
                        else std::fprintf(stderr, "wrote %s after %d frames (%.1fs)\n", shot.c_str(), frame, secs);
                        running = false;
                    }
                }
            }
        }
        vkDeviceWaitIdle(vk.device);
    }
    fog.shutdown();
    ImGui_ImplVulkan_Shutdown();
    if (window) ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    vk.shutdown();
    if (window) { SDL_DestroyWindow(window); SDL_Quit(); }
    return rc;
}
