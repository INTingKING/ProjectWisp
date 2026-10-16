#include "data/tracks.h"

#include <htslib/hts.h>
#include <htslib/kstring.h>
#include <htslib/tbx.h>

extern "C" {
#include <bigWig.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <tuple>

namespace wisp {

namespace {
const char* kFiles[4] = {"genes.gff.gz", "ccre.bed.gz", "repeats.bed.gz", "notgrch38.bed.gz"};

int split(char* s, char* f[], int maxf) {
    int n = 0;
    f[n++] = s;
    for (char* p = s; *p && n < maxf; ++p)
        if (*p == '\t') { *p = 0; f[n++] = p + 1; }
    return n;
}
std::string attr(const char* a, const char* key) {
    size_t kl = std::strlen(key);
    for (const char* p = a; p && *p;) {
        if (std::strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char* v = p + kl + 1;
            const char* e = std::strchr(v, ';');
            return e ? std::string(v, e) : std::string(v);
        }
        p = std::strchr(p, ';');
        if (p) ++p;
    }
    return {};
}
}  // namespace

TrackStore::~TrackStore() {
    {
        std::lock_guard lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (bw_) bwClose(static_cast<bigWigFile_t*>(bw_));
    for (int i = 0; i < kTrackCount; ++i) {
        if (tbx_[i]) tbx_destroy(tbx_[i]);
        if (fp_[i]) hts_close(fp_[i]);
    }
}

bool TrackStore::open(const std::string& dir, const std::string& variantVcf, std::string& err) {
    for (int i = 0; i < 4; ++i) {
        paths_[i] = dir + "/" + kFiles[i];
        fp_[i] = hts_open(paths_[i].c_str(), "r");
        tbx_[i] = fp_[i] ? tbx_index_load(paths_[i].c_str()) : nullptr;
        if (!fp_[i] || !tbx_[i]) { err = "cannot open tabix track " + paths_[i] + " (run scripts/prepare_tracks.sh)"; return false; }
    }
    auto openOptional = [&](Track t, const std::string& path) {
        int v = int(t);
        paths_[v] = path;
        fp_[v] = hts_open(path.c_str(), "r");
        tbx_[v] = fp_[v] ? tbx_index_load(path.c_str()) : nullptr;
        if (!tbx_[v] && fp_[v]) { hts_close(fp_[v]); fp_[v] = nullptr; }
    };
    openOptional(Track::Sv, dir + "/sv.hprc.vcf.gz");  // scripts/fetch_sv.sh; optional
    {
        std::string bw = dir + "/chm13v2.0_CpG_methylation.bw";  // optional, fetch_data.sh
        FILE* t = std::fopen(bw.c_str(), "rb");
        if (t) {
            std::fclose(t);
            bw_ = bwOpen(bw.c_str(), nullptr, "r");
        }
    }
    if (!variantVcf.empty()) {
        int v = int(Track::Variants);
        paths_[v] = variantVcf;
        fp_[v] = hts_open(variantVcf.c_str(), "r");
        tbx_[v] = fp_[v] ? tbx_index_load(variantVcf.c_str()) : nullptr;
        if (!tbx_[v]) {  // optional: run without variants
            if (fp_[v]) hts_close(fp_[v]);
            fp_[v] = nullptr;
        } else {
            int n = 0;
            const char** names = tbx_seqnames(tbx_[v], &n);
            for (int i = 0; i < n; ++i) varContigs_.push_back(names[i]);
            free(names);
        }
    }
    thread_ = std::thread([this] { worker(); });
    return true;
}

TilePtr TrackStore::request(Track t, const std::string& chrom, int64_t tile) {
    std::lock_guard lk(mu_);
    Key k{int(t), chrom, tile};
    auto it = map_.find(k);
    if (it != map_.end()) {
        it->second.lastUse = ++clock_;
        if (it->second.tile) return it->second.tile;
        // still queued: move to the back so the freshest requests load first
        auto q = std::find(queue_.begin(), queue_.end(), k);
        if (q != queue_.end()) { queue_.erase(q); queue_.push_back(k); }
        return nullptr;
    }
    map_[k] = Entry{nullptr, ++clock_, false};
    queue_.push_back(k);
    if (queue_.size() > 64) {  // drop stale requests (view moved on)
        map_.erase(queue_.front());
        queue_.erase(queue_.begin());
    }
    cv_.notify_one();
    return nullptr;
}

size_t TrackStore::pending() const {
    std::lock_guard lk(mu_);
    return queue_.size() + (busy_ ? 1 : 0);
}

size_t TrackStore::bytes() const {
    std::lock_guard lk(mu_);
    return bytes_;
}

void TrackStore::worker() {
    for (;;) {
        Key k;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            k = queue_.back();
            queue_.pop_back();
            busy_ = true;
        }
        auto tile = load(k);
        std::lock_guard lk(mu_);
        busy_ = false;
        auto it = map_.find(k);
        if (it == map_.end()) continue;
        it->second.tile = tile;
        bytes_ += tile->bytes;
        evictLocked();
    }
}

void TrackStore::evictLocked() {
    while (bytes_ > kBudget) {
        auto victim = map_.end();
        for (auto it = map_.begin(); it != map_.end(); ++it)
            if (it->second.tile && (victim == map_.end() || it->second.lastUse < victim->second.lastUse)) victim = it;
        if (victim == map_.end()) break;
        bytes_ -= victim->second.tile->bytes;
        map_.erase(victim);
    }
}

std::shared_ptr<Tile> TrackStore::load(const Key& k) {
    auto tile = std::make_shared<Tile>();
    int t = k.t;
    if (Track(t) == Track::Methyl) {
        if (!bw_) return tile;
        bwOverlappingIntervals_t* o = bwGetOverlappingIntervals(static_cast<bigWigFile_t*>(bw_), k.chrom.c_str(), uint32_t(k.tile * kTile), uint32_t((k.tile + 1) * kTile));
        if (o) {
            tile->methyl.reserve(o->l);
            for (uint32_t i = 0; i < o->l; ++i) tile->methyl.push_back({o->start[i], o->end[i], o->value[i]});
            bwDestroyOverlappingIntervals(o);
        }
        tile->bytes = sizeof(Tile) + tile->methyl.size() * sizeof(MethylSite);
        return tile;
    }
    if (!tbx_[t]) return tile;
    int tid = tbx_name2id(tbx_[t], k.chrom.c_str());
    if (tid < 0) return tile;
    hts_itr_t* itr = tbx_itr_queryi(tbx_[t], tid, k.tile * kTile, (k.tile + 1) * kTile);
    if (!itr) return tile;
    kstring_t s = {0, 0, nullptr};
    char* f[12];
    size_t bytes = 0;
    while (tbx_itr_next(fp_[t], tbx_[t], itr, &s) >= 0) {
        int n = split(s.s, f, 12);
        bytes += s.l + 64;
        switch (Track(t)) {
            case Track::Genes:
                if (n < 9) break;
                if (std::strcmp(f[2], "gene") == 0)
                    tile->genes.push_back({std::atoll(f[3]) - 1, std::atoll(f[4]), f[6][0], attr(f[8], "gene"), attr(f[8], "biotype")});
                else
                    tile->exons.push_back({std::atoll(f[3]) - 1, std::atoll(f[4]), attr(f[8], "gene")});
                break;
            case Track::Ccre:
                if (n < 4) break;
                tile->ccres.push_back({std::atoll(f[1]), std::atoll(f[2]), f[3], n > 9 ? std::string(f[9]) : std::string()});
                break;
            case Track::Repeats:
                if (n < 7) break;
                tile->repeats.push_back({std::atoll(f[1]), std::atoll(f[2]), f[4][0], f[3], f[5], f[6]});
                break;
            case Track::NotIn:
                if (n < 3) break;
                tile->notin.push_back({std::atoll(f[1]), std::atoll(f[2])});
                break;
            case Track::Variants: {
                if (n < 8) break;
                int64_t pos = std::atoll(f[1]) - 1;
                const char* info = f[7];
                const char* afp = nullptr;
                if (std::strncmp(info, "AF=", 3) == 0) afp = info + 3;
                else if (const char* q = std::strstr(info, ";AF=")) afp = q + 4;
                std::string ref(f[3]);
                int64_t len = std::max<int64_t>(1, int64_t(ref.size()));
                if (ref.size() > 12) ref = ref.substr(0, 12) + "...";
                // split ALT and AF lists by allele index
                std::vector<std::string> alts;
                for (const char* a2 = f[4];;) {
                    const char* c = std::strchr(a2, ',');
                    alts.emplace_back(a2, c ? c : a2 + std::strlen(a2));
                    if (!c) break;
                    a2 = c + 1;
                }
                std::vector<float> afs;
                for (const char* q = afp; q && *q && *q != ';';) {
                    afs.push_back(float(std::atof(q)));
                    while (*q && *q != ',' && *q != ';') ++q;
                    if (*q == ',') ++q;
                }
                for (size_t i = 0; i < alts.size(); ++i) {
                    std::string alt = alts[i];
                    if (alt.size() > 12) alt = alt.substr(0, 12) + "...";
                    float af = i < afs.size() ? afs[i] : -1.f;
                    tile->variants.push_back({pos, pos + len, af, ref, std::move(alt), int(i + 1), int(alts.size())});
                    bytes += sizeof(Variant);
                }
                bytes -= s.l;  // only the parsed records are kept, not the line
                break;
            }
            case Track::Sv: {
                if (n < 8) break;
                const char* info = f[7];
                auto field = [&](const char* key) -> const char* {
                    size_t kl = std::strlen(key);
                    for (const char* p = info; p && *p;) {
                        if (std::strncmp(p, key, kl) == 0 && (p[kl] == '=' || p[kl] == ';' || p[kl] == 0)) return p + kl;
                        p = std::strchr(p, ';');
                        if (p) ++p;
                    }
                    return nullptr;
                };
                int64_t pos = std::atoll(f[1]) - 1;
                int64_t refLen = int64_t(std::strlen(f[3])), altLen = int64_t(std::strlen(f[4]));
                const char* ty = field("TYPE");
                const char* ln = field("LEN");
                const char* af = field("AF");
                const char* ac = field("AC");
                const char* an = field("AN");
                bool inv = field("INV") != nullptr;
                SvType t2 = SvType::Complex;
                if (inv) t2 = SvType::Complex;
                else if (ty && std::strncmp(ty, "=ins", 4) == 0) t2 = SvType::Ins;
                else if (ty && std::strncmp(ty, "=del", 4) == 0) t2 = SvType::Del;
                int64_t len = ln ? std::llabs(std::atoll(ln + 1)) : std::llabs(altLen - refLen);
                int64_t e = t2 == SvType::Ins ? pos + 1 : pos + std::max<int64_t>(1, refLen);
                uint64_t h = 1469598103934665603ull;  // FNV-1a of the full ALT sequence
                for (const char* c = f[4]; *c; ++c) h = (h ^ uint8_t(*c)) * 1099511628211ull;
                Sv sv{pos, e, t2, len, refLen, altLen - refLen, af ? float(std::atof(af + 1)) : -1.f,
                      ac ? std::atoi(ac + 1) : 0, an ? std::atoi(an + 1) : 0};
                sv.inv = inv;
                sv.altHash = h;
                tile->svs.push_back(sv);
                bytes -= s.l;
                bytes += sizeof(Sv);
                break;
            }
            default: break;
        }
    }
    free(s.s);
    tbx_itr_destroy(itr);
    if (!tile->svs.empty()) {
        // 1) merge identical alleles (start + REF length + ALT hash): keep max AC and its AN
        auto& v = tile->svs;
        std::sort(v.begin(), v.end(), [](const Sv& a, const Sv& b) {
            return std::tie(a.s, a.refLen, a.altHash, b.ac) < std::tie(b.s, b.refLen, b.altHash, a.ac);
        });
        std::vector<Sv> out;
        for (auto& x : v) {
            if (!out.empty() && out.back().s == x.s && out.back().refLen == x.refLen && out.back().altHash == x.altHash) {
                ++out.back().merged;
                continue;
            }
            out.push_back(x);
        }
        bytes -= (v.size() - out.size()) * sizeof(Sv);
        v.swap(out);
        // 2) group distinct alleles by start: AC summed (capped at AN), AN = group max
        for (size_t i = 0; i < v.size();) {
            size_t j = i;
            SvGroup g{v[i].s, v[i].e, SvType::Ins};
            bool anyDel = false, anyCx = false;
            while (j < v.size() && v[j].s == v[i].s) {
                const Sv& x = v[j];
                g.e = std::max(g.e, x.e);
                g.ac += x.ac;
                g.an = std::max(g.an, x.an);
                g.inv |= x.inv;
                g.atLeast |= x.merged > 1;
                anyDel |= x.type == SvType::Del;
                anyCx |= x.type == SvType::Complex || x.type == SvType::Inv;
                ++j;
            }
            g.type = anyCx ? SvType::Complex : anyDel ? SvType::Del : SvType::Ins;
            if (g.an > 0) g.ac = std::min(g.ac, g.an);
            g.alleles = &v[i];
            g.count = int(j - i);
            tile->svGroups.push_back(g);
            i = j;
        }
        bytes += tile->svGroups.size() * sizeof(SvGroup);
    }
    tile->bytes = bytes + sizeof(Tile);
    return tile;
}

}  // namespace wisp
