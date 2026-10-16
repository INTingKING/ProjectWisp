#pragma once
// Per-bin genome summary (scripts/make_overview.py, format WISPOV1). A few hundred KB, loaded whole.
#include <cstdint>
#include <string>
#include <vector>

namespace wisp {

enum Cat : int { kExon = 0, kCcre, kIntron, kRepeat, kNothing, kNotIn, kCatCount };

struct OverviewChrom {
    std::string name;
    uint64_t length = 0;
    uint32_t nbins = 0;
    size_t offset = 0;  // first bin index into Overview::bins_
    uint64_t bp[6] = {};  // exact per-chromosome bp per category (WISPOV2); estimated from bins for WISPOV1
    double share(Cat c) const { return length ? double(bp[c]) / double(length) : 0.0; }
};

class Overview {
public:
    bool load(const std::string& path, std::string& err);
    uint32_t binSize() const { return binSize_; }
    const std::vector<OverviewChrom>& chroms() const { return chroms_; }
    int find(const std::string& name) const;
    // Genome-wide share (0..1) of a category; exact per-base totals from the file header.
    double share(Cat c) const { return totals_[6] ? double(totals_[c]) / double(totals_[6]) : 0.0; }
    uint64_t total(Cat c) const { return totals_[c]; }
    uint64_t genomeLength() const { return totals_[6]; }
    // Fraction of a category in bin b of chromosome ci.
    float frac(int ci, uint32_t b, Cat c) const;
    // Length-weighted average fractions over [a, b) bp of chromosome ci.
    void average(int ci, double a, double b, float out[kCatCount]) const;
    // Approximate share of each category that is also notInGRCh38 (assumes independence inside a bin).
    void notInByCategory(double out[kCatCount]) const;

private:
    uint32_t binSize_ = 100000;
    uint64_t totals_[7] = {};
    std::vector<OverviewChrom> chroms_;
    std::vector<uint16_t> bins_;  // 6 per bin
};

}  // namespace wisp
