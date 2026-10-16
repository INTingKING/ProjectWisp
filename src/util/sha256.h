#pragma once
// SHA-256 (FIPS 180-4), streaming. Written for ProjectWisp and dedicated to the public domain (CC0 / Unlicense):
// no copyright is claimed; use it for anything. Used on every platform instead of shelling out to sha256sum.
#include <cstddef>
#include <cstdint>
#include <string>

namespace wisp {

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    std::string hexdigest();  // finalises; call reset() to reuse
private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint64_t bits_ = 0;
    uint8_t buf_[64];
    size_t n_ = 0;
};

std::string sha256File(const std::string& path);  // empty on read error

}  // namespace wisp
