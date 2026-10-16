#include "data/geneindex.h"

#include <htslib/hts.h>
#include <htslib/kstring.h>

#include <algorithm>
#include <cctype>

namespace wisp {

static std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

bool GeneIndex::load(const std::string& path, std::string& err) {
    htsFile* f = hts_open(path.c_str(), "r");
    if (!f) { err = "cannot open " + path; return false; }
    kstring_t s = {0, 0, nullptr};
    while (hts_getline(f, '\n', &s) >= 0) {
        std::vector<std::string> c;
        std::string cur;
        for (size_t i = 0; i < s.l; ++i) {
            if (s.s[i] == '\t') { c.push_back(cur); cur.clear(); } else cur.push_back(s.s[i]);
        }
        c.push_back(cur);
        if (c.size() < 7) continue;
        GeneEntry g;
        g.name = c[0];
        for (size_t p = 0; p < c[1].size();) {
            size_t q = c[1].find(',', p);
            if (q == std::string::npos) q = c[1].size();
            if (q > p) g.aliases.push_back(c[1].substr(p, q - p));
            p = q + 1;
        }
        g.chrom = c[2];
        g.s = std::atoll(c[3].c_str());
        g.e = std::atoll(c[4].c_str());
        g.strand = c[5].empty() ? '.' : c[5][0];
        g.extra = c[6] == "1";
        genes_.push_back(std::move(g));
    }
    free(s.s);
    hts_close(f);
    for (auto& g : genes_) {
        lname_.push_back(lower(g.name));
        std::vector<std::string> la;
        for (auto& a : g.aliases) la.push_back(lower(a));
        lalias_.push_back(std::move(la));
    }
    return true;
}

size_t GeneIndex::aliasCount() const { size_t n = 0; for (auto& g : genes_) n += g.aliases.size(); return n; }
size_t GeneIndex::extraCount() const { size_t n = 0; for (auto& g : genes_) n += g.extra; return n; }

std::vector<GeneHit> GeneIndex::search(const std::string& query, size_t max) const {
    std::string q = lower(query);
    while (!q.empty() && q.back() == ' ') q.pop_back();
    while (!q.empty() && q.front() == ' ') q.erase(q.begin());
    std::vector<GeneHit> out;
    if (q.empty()) return out;
    for (size_t i = 0; i < genes_.size(); ++i) {
        if (lname_[i] == q) out.push_back({&genes_[i], 0, {}});
        else if (lname_[i].rfind(q, 0) == 0) out.push_back({&genes_[i], 1, {}});
        else
            for (size_t k = 0; k < lalias_[i].size(); ++k)
                if (lalias_[i][k].rfind(q, 0) == 0) { out.push_back({&genes_[i], 2, genes_[i].aliases[k]}); break; }
    }
    std::sort(out.begin(), out.end(), [](const GeneHit& a, const GeneHit& b) {
        if (a.tier != b.tier) return a.tier < b.tier;
        if (a.g->name.size() != b.g->name.size()) return a.g->name.size() < b.g->name.size();
        if (a.g->name != b.g->name) return a.g->name < b.g->name;
        return a.g->extra < b.g->extra;
    });
    // extra copies directly under their main copy (same symbol, best tier of the family)
    std::vector<GeneHit> ord;
    std::vector<bool> used(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (used[i] || out[i].g->extra) continue;
        used[i] = true;
        ord.push_back(out[i]);
        for (size_t j = 0; j < out.size(); ++j)
            if (!used[j] && out[j].g->extra && out[j].g->name == out[i].g->name) { used[j] = true; ord.push_back(out[j]); }
    }
    for (size_t i = 0; i < out.size(); ++i) if (!used[i]) ord.push_back(out[i]);  // copies with no main copy
    out.swap(ord);
    if (out.size() > max) out.resize(max);
    return out;
}

}  // namespace wisp
