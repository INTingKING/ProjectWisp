#pragma once
// Region-indexed annotation tracks read through htslib/tabix in 1 Mb tiles on a background thread.
// Only visible tiles are requested; the cache is LRU-evicted to stay under ~100 MB.
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

typedef struct htsFile htsFile;
typedef struct tbx_t tbx_t;

namespace wisp {

enum class Track : int { Genes = 0, Ccre, Repeats, NotIn, Variants, Sv, Methyl, Count };
constexpr int kTrackCount = int(Track::Count);

struct Interval { int64_t s, e; };
struct Gene { int64_t s, e; char strand; std::string name, biotype; };
struct Exon { int64_t s, e; std::string gene; };
struct Ccre { int64_t s, e; std::string id, cls; };
struct Variant {  // one per ALT allele (multi-allelic sites split at parse time); e = s + len(REF)
    int64_t s, e;
    float af;  // AF for this allele index, -1 if absent
    std::string ref, alt;
    int alleleIndex = 1, alleleCount = 1;
};
enum class SvType : uint8_t { Ins, Del, Inv, Complex };  // Inv kept for API compatibility; INV-flagged records load as Complex
struct Sv {  // one distinct ALT allele (identical records merged at load: max AC + its AN)
    int64_t s, e;     // ref span (REF bubble); insertions are a 1 bp anchor
    SvType type;
    int64_t len;      // |INFO/LEN|
    int64_t refLen;   // REF length
    int64_t net;      // len(ALT) - len(REF)
    float af;         // over chromosome copies, -1 if absent
    int ac = 0, an = 0;
    int merged = 1;   // identical records merged into this one
    bool inv = false; // INV flag: complex rearrangement containing an inversion
    uint64_t altHash = 0;
};
struct SvGroup {  // distinct alleles sharing a start position
    int64_t s, e;     // e = max end over alleles
    SvType type;      // drawing type: Complex if any complex, else Del if any del, else Ins
    bool inv = false; // any allele contains an inversion
    int ac = 0, an = 0;  // AC summed over alleles (capped at AN), AN = group max
    bool atLeast = false;  // some allele was merged from identical records
    const Sv* alleles = nullptr;
    int count = 0;
};
struct MethylSite { uint32_t s, e; float frac; };  // CpG methylation fraction 0..1 (bigWig)
struct Repeat { int64_t s, e; char strand; std::string name, cls, family; };

struct Tile {
    std::vector<Gene> genes;
    std::vector<Exon> exons;
    std::vector<Ccre> ccres;
    std::vector<Repeat> repeats;
    std::vector<Interval> notin;
    std::vector<Variant> variants;
    std::vector<Sv> svs;
    std::vector<SvGroup> svGroups;  // point into svs (stable once the tile is built)
    std::vector<MethylSite> methyl;
    size_t bytes = 0;
};
using TilePtr = std::shared_ptr<const Tile>;

class TrackStore {
public:
    static constexpr int64_t kTile = 1'000'000;
    static constexpr size_t kBudget = size_t(100) << 20;

    ~TrackStore();
    // variantVcf: optional bgzipped+tabixed VCF (empty = no variants track)
    bool open(const std::string& trackDir, const std::string& variantVcf, std::string& err);
    bool hasVariants() const { return tbx_[int(Track::Variants)] != nullptr; }
    bool hasSv() const { return tbx_[int(Track::Sv)] != nullptr; }
    bool hasMethyl() const { return bw_ != nullptr; }
    // Returns the tile if cached, otherwise queues it and returns nullptr.
    TilePtr request(Track t, const std::string& chrom, int64_t tile);
    size_t pending() const;
    size_t bytes() const;

private:
    struct Key {
        int t;
        std::string chrom;
        int64_t tile;
        auto operator<=>(const Key&) const = default;
    };
    struct Entry { TilePtr tile; uint64_t lastUse = 0; bool loading = false; };
    void worker();
    std::shared_ptr<Tile> load(const Key& k);
    void evictLocked();

    std::string paths_[kTrackCount];
    htsFile* fp_[kTrackCount] = {};
    tbx_t* tbx_[kTrackCount] = {};
    void* bw_ = nullptr;  // methylation bigWig, read per tile by the worker only
    std::map<Key, Entry> map_;
    std::vector<Key> queue_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    bool stop_ = false;
    bool busy_ = false;
    uint64_t clock_ = 0;
    size_t bytes_ = 0;
};

}  // namespace wisp
