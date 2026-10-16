#include "data/rsid.h"

#include <cstring>

namespace wisp {

RsidIndex::~RsidIndex() { if (f_) std::fclose(f_); }

bool RsidIndex::open(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[8];
    uint32_t nc = 0;
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "WISPRS1", 8) != 0 || std::fread(&nc, 4, 1, f) != 1 ||
        std::fread(&n_, 4, 1, f) != 1 || std::fread(&recOff_, 8, 1, f) != 1 || std::fread(&blobOff_, 8, 1, f) != 1) {
        std::fclose(f);
        return false;
    }
    for (uint32_t i = 0; i < nc; ++i) {
        unsigned char l = 0;
        if (std::fread(&l, 1, 1, f) != 1) { std::fclose(f); return false; }
        std::string s(l, '\0');
        if (l && std::fread(s.data(), 1, l, f) != l) { std::fclose(f); return false; }
        chroms_.push_back(s);
    }
    f_ = f;
    return true;
}

bool RsidIndex::find(uint32_t rs, RsidHit& out) const {
    if (!f_ || !n_) return false;
    struct Rec { uint32_t rs, pos; uint8_t chrom, rl, al, pad; uint32_t off; } r{};
    static_assert(sizeof(Rec) == 16);
    uint32_t lo = 0, hi = n_;
    while (lo < hi) {  // ~25 seeks for tens of millions of records
        uint32_t mid = lo + (hi - lo) / 2;
        std::fseek(f_, long(recOff_ + uint64_t(mid) * 16), SEEK_SET);
        if (std::fread(&r, 16, 1, f_) != 1) return false;
        if (r.rs < rs) lo = mid + 1;
        else if (r.rs > rs) hi = mid;
        else {
            std::string al(size_t(r.rl) + r.al, '\0');
            std::fseek(f_, long(blobOff_ + r.off), SEEK_SET);
            if (!al.empty() && std::fread(al.data(), 1, al.size(), f_) != al.size()) return false;
            out.rs = rs;
            out.chrom = r.chrom < chroms_.size() ? chroms_[r.chrom] : "?";
            out.pos = r.pos;
            out.ref = al.substr(0, r.rl);
            out.alt = al.substr(r.rl);
            return true;
        }
    }
    return false;
}

}  // namespace wisp
