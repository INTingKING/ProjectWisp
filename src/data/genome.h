#pragma once
// Reference sequence via htslib faidx on the bgzipped FASTA. Only small windows are ever fetched.
#include <cstdint>
#include <mutex>
#include <string>

typedef struct faidx_t faidx_t;

namespace wisp {

class Genome {
public:
    ~Genome();
    bool open(const std::string& fastaPath, std::string& err);
    // Bases [s, e) (0-based) of chrom; soft-masked case preserved. Empty on error. Spans > 1 Mb refused.
    std::string fetch(const std::string& chrom, int64_t s, int64_t e);

private:
    faidx_t* fai_ = nullptr;
    std::mutex mu_;
    std::string cChrom_, cSeq_;
    int64_t cS_ = 0, cE_ = 0;
};

}  // namespace wisp
