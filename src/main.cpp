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

// ---------- --script FILE: replay SDL events through the normal event loop (offscreen) ----------
// One command per line ('#' comments):
//   args <cli args...>      extra command-line args (e.g. --region chr22:... --pin ...), applied before start
//   outdir DIR              where shots go (default shots/review, or --shots-dir)
//   move X Y | down [left|right] | up [left|right] | click X Y | wheel DY
//   key [ctrl+][shift+]NAME (up down left right enter escape backspace tab slash a-z 0-9) | type TEXT
//   wait MS                 advance MS of simulated time (60 fps frames)
//   idle                    wait until tiles/animations/downloads settle
//   shot NAME               wait until idle, then write NAME.png;  shotnow NAME: write after 2 frames, no idle wait
//   quit
struct ScriptCmd { std::string op; std::vector<std::string> a; std::string rest; };
static const Uint32 kTextEvent = SDL_EVENT_USER + 1;

static bool loadScript(const std::string& path, std::vector<ScriptCmd>& cmds, std::vector<std::string>& extraArgs) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[1024];
    while (std::fgets(buf, sizeof buf, f)) {
        std::string l = buf;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        size_t b = l.find_first_not_of(" \t");
        if (b == std::string::npos || l[b] == '#') continue;
        l = l.substr(b);
        ScriptCmd c;
        size_t sp = l.find(' ');
        c.op = l.substr(0, sp);
        c.rest = sp == std::string::npos ? "" : l.substr(sp + 1);
        std::string tok;
        bool q = false;
        for (char ch : c.rest + " ") {  // split on spaces, "double quotes" group
            if (ch == '"') { q = !q; continue; }
            if (ch == ' ' && !q) { if (!tok.empty()) c.a.push_back(tok), tok.clear(); }
            else tok.push_back(ch);
        }
        if (c.op == "args") extraArgs.insert(extraArgs.end(), c.a.begin(), c.a.end());
        else cmds.push_back(c);
    }
    std::fclose(f);
    return true;
}

static SDL_Keycode keycodeFor(const std::string& n) {
    if (n == "up") return SDLK_UP;
    if (n == "down") return SDLK_DOWN;
    if (n == "left") return SDLK_LEFT;
    if (n == "right") return SDLK_RIGHT;
    if (n == "enter") return SDLK_RETURN;
    if (n == "escape") return SDLK_ESCAPE;
    if (n == "backspace") return SDLK_BACKSPACE;
    if (n == "tab") return SDLK_TAB;
    if (n == "slash") return SDLK_SLASH;
    if (n.size() == 1 && ((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= '0' && n[0] <= '9'))) return SDL_Keycode(n[0]);
    return SDLK_UNKNOWN;
}

static ImGuiKey imguiKeyFor(SDL_Keycode k) {
    switch (k) {
        case SDLK_UP: return ImGuiKey_UpArrow;
        case SDLK_DOWN: return ImGuiKey_DownArrow;
        case SDLK_LEFT: return ImGuiKey_LeftArrow;
        case SDLK_RIGHT: return ImGuiKey_RightArrow;
        case SDLK_RETURN: return ImGuiKey_Enter;
        case SDLK_ESCAPE: return ImGuiKey_Escape;
        case SDLK_BACKSPACE: return ImGuiKey_Backspace;
        case SDLK_TAB: return ImGuiKey_Tab;
        case SDLK_SLASH: return ImGuiKey_Slash;
        default: break;
    }
    if (k >= 'a' && k <= 'z') return ImGuiKey(ImGuiKey_A + int(k - 'a'));
    if (k >= '0' && k <= '9') return ImGuiKey(ImGuiKey_0 + int(k - '0'));
    return ImGuiKey_None;
}

// windowless offscreen mode: feed SDL events to Dear ImGui (the SDL3 backend needs a window)
static void feedImGui(const SDL_Event& e, ImGuiIO& io) {
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION: io.AddMousePosEvent(e.motion.x, e.motion.y); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            io.AddMousePosEvent(e.button.x, e.button.y);
            io.AddMouseButtonEvent(e.button.button == SDL_BUTTON_RIGHT ? 1 : 0, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        case SDL_EVENT_MOUSE_WHEEL: io.AddMouseWheelEvent(e.wheel.x, e.wheel.y); break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            bool d = e.type == SDL_EVENT_KEY_DOWN;
            // modifiers are held only for the duration of the key press (released together with the key)
            if (d) {
                io.AddKeyEvent(ImGuiMod_Ctrl, (e.key.mod & SDL_KMOD_CTRL) != 0);
                io.AddKeyEvent(ImGuiMod_Shift, (e.key.mod & SDL_KMOD_SHIFT) != 0);
            }
            io.AddKeyEvent(imguiKeyFor(e.key.key), d);
            if (!d) { io.AddKeyEvent(ImGuiMod_Ctrl, false); io.AddKeyEvent(ImGuiMod_Shift, false); }
            break;
        }
        default:
            if (e.type == kTextEvent && e.user.data1) {
                auto* t = static_cast<std::string*>(e.user.data1);
                if (std::getenv("WISP_SCRIPT_DEBUG")) std::fprintf(stderr, "text event: %s\n", t->c_str());
                io.AddInputCharactersUTF8(t->c_str());
                delete t;
            }
    }
}

int main(int argc, char** argv) {
    AppOptions opt;
    std::string scriptPath, shotsDir = "shots/review";
    std::vector<ScriptCmd> script;
    std::vector<std::string> args(argv + 1, argv + argc);
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--script") scriptPath = args[i + 1];
    if (!scriptPath.empty()) {
        std::vector<std::string> extra;
        if (!loadScript(scriptPath, script, extra)) { std::fprintf(stderr, "cannot read script %s\n", scriptPath.c_str()); return 1; }
        args.insert(args.begin(), extra.begin(), extra.end());  // explicit CLI args win (parsed later)
    }
    std::string shot;
    int W = 1600, H = 1000;
    float shotTime = 2.0f;
    float mouseX = -1, mouseY = -1;  // --mouse X,Y: simulated cursor for offscreen shots (hover/tooltips)
    for (size_t i = 0; i < args.size(); ++i) {
        std::string a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : ""; };
        if (a == "--data") opt.dataDir = next();
        else if (a == "--region") opt.region = next();
        else if (a == "--pin") opt.pin = next();
        else if (a == "--variants") opt.variants = next();
        else if (a == "--search") opt.search = next();
        else if (a == "--rsid-pack") opt.rsidPack = next();
        else if (a == "--pack-card") opt.packCard = true;
        else if (a == "--pack-download") opt.packDownload = true;
        else if (a == "--search-group") opt.searchGroup = next();
        else if (a == "--search-more") opt.searchMore = true;
        else if (a == "--goto") opt.gotoQuery = next();
        else if (a == "--expand-card") opt.expandCard = true;
        else if (a == "--expand") { std::string t = next(); if (t == "sv") opt.expandSv = true; }
        else if (a == "--reduced-motion") opt.reducedMotion = true;
        else if (a == "--shot") shot = next();
        else if (a == "--script") next();
        else if (a == "--shots-dir") shotsDir = next();
        else if (a == "--mouse") std::sscanf(next().c_str(), "%f,%f", &mouseX, &mouseY);
        else if (a == "--time") shotTime = std::strtof(next().c_str(), nullptr);
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &W, &H);
        else if (a == "-h" || a == "--help") {
            std::printf("usage: wisp [--data DIR] [--region LOCUS] [--pin LOCUS] [--reduced-motion]\n"
                        "            [--shot out.png --size WxH --time SECONDS --mouse X,Y]\n");
            return 0;
        }
    }
    const bool scripted = !scriptPath.empty();
    const bool offscreen = !shot.empty() || scripted;
    if (scripted && !SDL_Init(SDL_INIT_EVENTS)) { std::fprintf(stderr, "SDL_Init(events): %s\n", SDL_GetError()); return 1; }

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
    opt.assetsDir = findAssets(argv[0]);
    theme::apply(opt.assetsDir.c_str());
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
            // script state
            size_t pc = 0;
            int waitFrames = 0;
            bool waitIdle = false;
            std::string pendingShot;
            int shotDelay = 0;
            float mx = mouseX, my = mouseY;
            int shotsWritten = 0;
            auto pushMouse = [&](Uint32 type, Uint8 btn) {
                SDL_Event e{};
                e.type = type;
                if (type == SDL_EVENT_MOUSE_MOTION) { e.motion.x = mx; e.motion.y = my; }
                else { e.button.x = mx; e.button.y = my; e.button.button = btn; e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN; }
                SDL_PushEvent(&e);
            };
            auto stepScript = [&]() {  // run commands until one needs time to pass
                while (pc < script.size() && waitFrames == 0 && !waitIdle && pendingShot.empty()) {
                    const ScriptCmd& c = script[pc++];
                    auto num = [&](size_t k) { return k < c.a.size() ? std::strtof(c.a[k].c_str(), nullptr) : 0.f; };
                    Uint8 btn = (!c.a.empty() && c.a[0] == "right") ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
                    if (c.op == "move") { mx = num(0); my = num(1); pushMouse(SDL_EVENT_MOUSE_MOTION, 0); waitFrames = 1; }
                    else if (c.op == "down") { pushMouse(SDL_EVENT_MOUSE_BUTTON_DOWN, btn); waitFrames = 1; }
                    else if (c.op == "up") { pushMouse(SDL_EVENT_MOUSE_BUTTON_UP, btn); waitFrames = 1; }
                    else if (c.op == "click") {
                        mx = num(0); my = num(1);
                        pushMouse(SDL_EVENT_MOUSE_MOTION, 0);
                        pushMouse(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
                        pushMouse(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
                        waitFrames = 2;
                    } else if (c.op == "wheel") {
                        SDL_Event e{}; e.type = SDL_EVENT_MOUSE_WHEEL; e.wheel.y = num(0); e.wheel.mouse_x = mx; e.wheel.mouse_y = my;
                        SDL_PushEvent(&e); waitFrames = 1;
                    } else if (c.op == "key") {
                        std::string n = c.rest;
                        SDL_Keymod mod = 0;
                        for (;;) {
                            if (n.rfind("ctrl+", 0) == 0) { mod |= SDL_KMOD_LCTRL; n = n.substr(5); }
                            else if (n.rfind("shift+", 0) == 0) { mod |= SDL_KMOD_LSHIFT; n = n.substr(6); }
                            else break;
                        }
                        SDL_Event e{}; e.type = SDL_EVENT_KEY_DOWN; e.key.key = keycodeFor(n); e.key.mod = mod; e.key.down = true;
                        SDL_PushEvent(&e);
                        SDL_Event u = e; u.type = SDL_EVENT_KEY_UP; u.key.down = false;
                        SDL_PushEvent(&u);
                        waitFrames = 2;
                    } else if (c.op == "type") {
                        SDL_Event e{}; e.type = kTextEvent; e.user.data1 = new std::string(c.rest);
                        SDL_PushEvent(&e); waitFrames = 2;
                    } else if (c.op == "wait") waitFrames = std::max(1, int(num(0) / (1000.f / 60.f) + 0.5f));
                    else if (c.op == "idle") waitIdle = true;
                    else if (c.op == "shot" || c.op == "shotnow") {
                        pendingShot = c.a.empty() ? "shot" : c.a[0];
                        shotDelay = c.op == "shotnow" ? 2 : -1;
                    } else if (c.op == "outdir") shotsDir = c.rest;
                    else if (c.op == "quit") pc = script.size();
                    else std::fprintf(stderr, "script: unknown command '%s'\n", c.op.c_str());
                }
            };
            while (running) {
                if (scripted) {
                    stepScript();
                    SDL_Event e;
                    while (SDL_PollEvent(&e)) feedImGui(e, io);
                }
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
                    if (!scripted) io.MousePos = ImVec2(mouseX, mouseY);
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
                if (scripted) {
                    ++frame;
                    idleFrames = app.idle() ? idleFrames + 1 : 0;
                    if (waitFrames > 0) --waitFrames;
                    if (waitIdle && (idleFrames >= 3 || secs > 120.f)) waitIdle = false;
                    bool take = false;
                    if (!pendingShot.empty()) {
                        if (shotDelay >= 0) take = shotDelay-- <= 0;
                        else take = idleFrames >= 3;
                    }
                    if (take) {
                        std::filesystem::create_directories(shotsDir);
                        std::string path = pendingShot.find('/') == std::string::npos ? shotsDir + "/" + pendingShot : pendingShot;
                        if (path.size() < 4 || path.substr(path.size() - 4) != ".png") path += ".png";
                        std::vector<uint8_t> px;
                        vk.readback(px);
                        if (writePng(path, int(vk.extent.width), int(vk.extent.height), px)) { std::fprintf(stderr, "wrote %s (frame %d)\n", path.c_str(), frame); ++shotsWritten; }
                        else { std::fprintf(stderr, "png write failed: %s\n", path.c_str()); rc = 1; }
                        pendingShot.clear();
                    }
                    if (pc >= script.size() && pendingShot.empty() && waitFrames == 0 && !waitIdle) running = false;
                    if (secs > 600.f) { std::fprintf(stderr, "script timeout\n"); rc = 1; running = false; }
                } else if (offscreen) {
                    ++frame;
                    idleFrames = app.idle() ? idleFrames + 1 : 0;
                    if ((frame >= 4 && idleFrames >= 3) || secs > 90.f) {
                        std::vector<uint8_t> px;
                        vk.readback(px);
                        if (!writePng(shot, int(vk.extent.width), int(vk.extent.height), px)) { std::fprintf(stderr, "png write failed\n"); rc = 1; }
                        else std::fprintf(stderr, "wrote %s after %d frames (%.1fs); SV track draw %.2f ms\n", shot.c_str(), frame, secs, app.svMs());
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
    else if (scripted) SDL_Quit();
    return rc;
}
