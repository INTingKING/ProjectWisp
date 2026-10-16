#pragma once
#include <imgui.h>

namespace wisp::theme {

constexpr ImU32 col(unsigned hex, float a = 1.f) {
    return IM_COL32((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, int(a * 255.f + 0.5f));
}
inline ImVec4 vec(unsigned hex, float a = 1.f) {
    return ImVec4(((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a);
}

// Designer palette
constexpr unsigned kBg = 0x0F1115, kPanel = 0x171A21, kHover = 0x1F232C, kBorder = 0x2A2F3A, kAccent = 0x7CC4FF,
                   kMuted = 0x8B93A1, kText = 0xE6E8EC;
constexpr unsigned kColKnown = 0x4C8DFF, kColCcre = 0xF2B544, kColIntron = 0x4A505C, kColRepeat = 0x6B7280, kColFog = 0xE9E4D8,
                   kColNotIn = 0x5B3A8C;

struct Fonts {
    ImFont* ui = nullptr;      // Inter 15
    ImFont* uiBold = nullptr;  // Inter SemiBold 15
    ImFont* title = nullptr;   // Inter SemiBold 20
    ImFont* mono = nullptr;    // JetBrains Mono 13
};
Fonts& fonts();
bool apply(const char* assetDir);

}  // namespace wisp::theme
