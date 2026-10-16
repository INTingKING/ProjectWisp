#include "data/overview.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace wisp {

bool Overview::load(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path + " (run scripts/prepare_tracks.sh)"; return false; }
    char magic[8];
    f.read(magic, 8);
    int version = std::memcmp(magic, "WISPOV2\0", 8) == 0 ? 2 : std::memcmp(magic, "WISPOV1\0", 8) == 0 ? 1 : 0;
    if (!version) { err = "bad overview magic in " + path; return false; }
    uint32_t n = 0;
    f.read(reinterpret_cast<char*>(&binSize_), 4);
    f.read(reinterpret_cast<char*>(&n), 4);
    f.read(reinterpret_cast<char*>(totals_), sizeof(totals_));
    size_t off = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t len = 0;
        f.read(reinterpret_cast<char*>(&len), 1);
        OverviewChrom c;
        c.name.resize(len);
        f.read(c.name.data(), len);
        f.read(reinterpret_cast<char*>(&c.length), 8);
        f.read(reinterpret_cast<char*>(&c.nbins), 4);
        if (version >= 2) f.read(reinterpret_cast<char*>(c.bp), sizeof(c.bp));
        c.offset = off;
        off += c.nbins;
        chroms_.push_back(std::move(c));
    }
    bins_.resize(off * 6);
    f.read(reinterpret_cast<char*>(bins_.data()), std::streamsize(bins_.size() * 2));
    if (!f) { err = "truncated overview file " + path; return false; }
    if (version == 1)  // older file: estimate per-chromosome totals from the bins
        for (auto& c : chroms_)
            for (uint32_t b = 0; b < c.nbins; ++b) {
                double size = std::min<double>(binSize_, double(c.length) - double(b) * binSize_);
                for (int k = 0; k < 6; ++k) c.bp[k] += uint64_t(bins_[(c.offset + b) * 6 + k] / 65535.0 * size + 0.5);
            }
    return true;
}

int Overview::find(const std::string& name) const {
    for (size_t i = 0; i < chroms_.size(); ++i)
        if (chroms_[i].name == name) return int(i);
    return -1;
}

float Overview::frac(int ci, uint32_t b, Cat c) const {
    const auto& ch = chroms_[ci];
    if (b >= ch.nbins) return 0.f;
    return bins_[(ch.offset + b) * 6 + c] / 65535.f;
}

void Overview::average(int ci, double a, double b, float out[kCatCount]) const {
    for (int k = 0; k < kCatCount; ++k) out[k] = 0.f;
    const auto& ch = chroms_[ci];
    a = std::max(0.0, a);
    b = std::min(double(ch.length), b);
    if (b <= a) return;
    uint32_t b0 = uint32_t(a / binSize_), b1 = std::min<uint32_t>(ch.nbins - 1, uint32_t((b - 1e-6) / binSize_));
    double wsum = 0;
    for (uint32_t i = b0; i <= b1; ++i) {
        double s = std::max(a, double(i) * binSize_), e = std::min(b, double(i + 1) * binSize_);
        double w = std::max(0.0, e - s);
        if (w <= 0) continue;
        const uint16_t* v = &bins_[(ch.offset + i) * 6];
        for (int k = 0; k < kCatCount; ++k) out[k] += float(w * v[k] / 65535.0);
        wsum += w;
    }
    if (wsum > 0)
        for (int k = 0; k < kCatCount; ++k) out[k] = float(out[k] / wsum);
}

void Overview::notInByCategory(double out[kCatCount]) const {
    for (int k = 0; k < kCatCount; ++k) out[k] = 0;
    for (size_t ci = 0; ci < chroms_.size(); ++ci) {
        const auto& ch = chroms_[ci];
        for (uint32_t b = 0; b < ch.nbins; ++b) {
            const uint16_t* v = &bins_[(ch.offset + b) * 6];
            double size = std::min<double>(binSize_, double(ch.length) - double(b) * binSize_);
            double u = v[kNotIn] / 65535.0;
            if (u <= 0) continue;
            for (int k = 0; k < kNotIn; ++k) out[k] += u * (v[k] / 65535.0) * size;
        }
    }
    if (totals_[6])
        for (int k = 0; k < kNotIn; ++k) out[k] /= double(totals_[6]);
}

}  // namespace wisp
