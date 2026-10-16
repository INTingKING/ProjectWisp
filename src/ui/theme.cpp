#include "ui/theme.h"

#include <cstdio>
#include <string>

namespace wisp::theme {

Fonts& fonts() {
    static Fonts f;
    return f;
}

bool apply(const char* assetDir) {
    ImGuiIO& io = ImGui::GetIO();
    std::string d = std::string(assetDir) + "/fonts/";
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    Fonts& f = fonts();
    f.ui = io.Fonts->AddFontFromFileTTF((d + "Inter-Regular.ttf").c_str(), 15.f, &cfg);
    f.uiBold = io.Fonts->AddFontFromFileTTF((d + "Inter-SemiBold.ttf").c_str(), 15.f, &cfg);
    f.title = io.Fonts->AddFontFromFileTTF((d + "Inter-SemiBold.ttf").c_str(), 20.f, &cfg);
    f.mono = io.Fonts->AddFontFromFileTTF((d + "JetBrainsMono-Regular.ttf").c_str(), 13.f, &cfg);
    f.monoSmall = io.Fonts->AddFontFromFileTTF((d + "JetBrainsMono-Regular.ttf").c_str(), 11.f, &cfg);
    f.monoTiny = io.Fonts->AddFontFromFileTTF((d + "JetBrainsMono-Regular.ttf").c_str(), 9.f, &cfg);
    bool ok = f.monoTiny && f.monoSmall && f.ui && f.uiBold && f.title && f.mono;
    if (!ok) {
        std::fprintf(stderr, "warning: fonts not found under %s, using ImGui default\n", d.c_str());
        ImFont* def = io.Fonts->AddFontDefault();
        if (!f.ui) f.ui = def;
        if (!f.uiBold) f.uiBold = def;
        if (!f.title) f.title = def;
        if (!f.mono) f.mono = def;
        if (!f.monoSmall) f.monoSmall = def;
        if (!f.monoTiny) f.monoTiny = def;
    }
    io.FontDefault = f.ui;

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 6;
    s.FrameRounding = 4;
    s.PopupRounding = 6;
    s.WindowBorderSize = 0;
    s.FrameBorderSize = 1;
    s.PopupBorderSize = 1;
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = vec(kText);
    c[ImGuiCol_TextDisabled] = vec(kMuted);
    c[ImGuiCol_WindowBg] = vec(kBg);
    c[ImGuiCol_ChildBg] = vec(kPanel, 0.f);
    c[ImGuiCol_PopupBg] = vec(kPanel);
    c[ImGuiCol_Border] = vec(kBorder);
    c[ImGuiCol_FrameBg] = vec(kBg);
    c[ImGuiCol_FrameBgHovered] = vec(kHover);
    c[ImGuiCol_FrameBgActive] = vec(kHover);
    c[ImGuiCol_Button] = vec(kHover);
    c[ImGuiCol_ButtonHovered] = vec(0x262B36);
    c[ImGuiCol_ButtonActive] = vec(0x2D3340);
    c[ImGuiCol_Header] = vec(kHover);
    c[ImGuiCol_HeaderHovered] = vec(0x262B36);
    c[ImGuiCol_Separator] = vec(kBorder);
    c[ImGuiCol_ScrollbarBg] = vec(kPanel);
    c[ImGuiCol_ScrollbarGrab] = vec(kBorder);
    c[ImGuiCol_TextSelectedBg] = vec(kAccent, 0.35f);
    c[ImGuiCol_NavHighlight] = vec(kAccent);
    return ok;
}

}  // namespace wisp::theme
