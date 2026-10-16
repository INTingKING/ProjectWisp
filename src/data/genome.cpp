#include "data/genome.h"

#include <htslib/faidx.h>

#include <algorithm>
#include <cstdlib>

namespace wisp {

Genome::~Genome() {
    if (fai_) fai_destroy(fai_);
}

bool Genome::open(const std::string& path, std::string& err) {
    fai_ = fai_load(path.c_str());
    if (!fai_) { err = "faidx: cannot load " + path + " (+ .fai/.gzi)"; return false; }
    return true;
}

std::string Genome::fetch(const std::string& chrom, int64_t s, int64_t e) {
    std::lock_guard lk(mu_);
    if (!fai_ || e <= s || e - s > 1'000'000) return {};
    if (chrom == cChrom_ && s >= cS_ && e <= cE_) return cSeq_.substr(size_t(s - cS_), size_t(e - s));
    int64_t pad = std::max<int64_t>(e - s, 2000);
    int64_t fs = std::max<int64_t>(0, s - pad), fe = e + pad;
    hts_pos_t len = 0;
    char* seq = faidx_fetch_seq64(fai_, chrom.c_str(), fs, fe - 1, &len);
    if (!seq) return {};
    cChrom_ = chrom;
    cSeq_.assign(seq, size_t(len));
    std::free(seq);
    cS_ = fs;
    cE_ = fs + len;
    if (s >= cE_) return {};
    return cSeq_.substr(size_t(s - cS_), size_t(std::min(e, cE_) - s));
}

}  // namespace wisp
