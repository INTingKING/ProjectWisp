#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "render/png_write.h"

namespace wisp {
bool writePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    return stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4) != 0;
}
}  // namespace wisp
