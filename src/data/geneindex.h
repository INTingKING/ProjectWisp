#pragma once
// Gene search index (data/tracks/genes.idx.tsv.gz, built by scripts/prepare_tracks.sh step 6).
#include <cstdint>
#include <string>
#include <vector>

namespace wisp {

struct GeneEntry {
    std::string name, chrom;
    std::vector<std::string> aliases;
    int64_t s = 0, e = 0;  // 0-based half-open
    char strand = '.';
    bool extra = false;  // Liftoff extra copy (extra_copy_number >= 1)
};

struct GeneHit {
    const GeneEntry* g;
    int tier;           // 0 exact symbol, 1 symbol prefix, 2 alias
    std::string alias;  // matched alias (tier 2)
};

class GeneIndex {
public:
    bool load(const std::string& path, std::string& err);
    std::vector<GeneHit> search(const std::string& q, size_t max) const;
    size_t size() const { return genes_.size(); }
    size_t aliasCount() const;
    size_t extraCount() const;
private:
    std::vector<GeneEntry> genes_;
    std::vector<std::string> lname_;                 // lower-case names
    std::vector<std::vector<std::string>> lalias_;   // lower-case aliases
};

}  // namespace wisp
