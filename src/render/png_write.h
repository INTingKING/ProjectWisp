#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace wisp {
bool writePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba);
}
