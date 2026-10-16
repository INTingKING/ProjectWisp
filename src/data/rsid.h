#pragma once
// Optional rsID pack (data/packs/rsid/rsid.idx, scripts/fetch_rsid_pack.sh): sorted 16-byte records,
// binary-searched straight from disk (never loaded whole).
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wisp {

struct RsidHit {
    uint32_t rs = 0;
    std::string chrom, ref, alt;  // alt may list several ALTs, comma separated
    int64_t pos = 0;              // 1-based
    int dropped = 0;              // ALTs in dbSNP not seen in 1000 Genomes (dropped at build time; WISPRS2)
};

class RsidIndex {
public:
    ~RsidIndex();
    bool open(const std::string& path);
    bool present() const { return f_ != nullptr; }
    uint32_t count() const { return n_; }
    bool find(uint32_t rs, RsidHit& out) const;
private:
    FILE* f_ = nullptr;
    uint32_t n_ = 0;
    int version_ = 1;
    uint64_t recOff_ = 0, blobOff_ = 0;
    std::vector<std::string> chroms_;
};

}  // namespace wisp
