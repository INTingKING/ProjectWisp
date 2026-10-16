#include "ui/app.h"
#include "util/download.h"
#include "util/sha256.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>

#include "ui/theme.h"

namespace wisp {

// measured from the built pack (scripts/fetch_rsid_pack.sh); shown on the download card
static const char* kRsidPackSize = "378 MB";
static const char* kRsidPackCount = "19.4 million";
static const char* kRsidPackStream = "24.9 GB";
static const char* kStorageNote = "Stored inside the app; uninstalling deletes it.";

using namespace theme;

namespace {
std::string mbStr(long long b);

constexpr int64_t kTrackMaxSpan = 3'000'000;  // above this the region view uses the 100 kb summary
constexpr float kCardW = 380.f, kMargin = 16.f, kOverviewH = 220.f, kTopBarH = 56.f;
const char* kNotInMsg =
    "New in T2T: this stretch isn't in the older GRCh38 genome, so most annotations don't reach it yet.";

std::string commas(int64_t v) {
    std::string s = std::to_string(v < 0 ? -v : v), o;
    int n = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (n && n % 3 == 0) o.push_back(',');
        o.push_back(*it);
        ++n;
    }
    if (v < 0) o.push_back('-');
    return {o.rbegin(), o.rend()};
}
std::string spanStr(double bp) {
    char b[32];
    if (bp >= 1e6) std::snprintf(b, sizeof b, "%.2f Mb", bp / 1e6);
    else if (bp >= 1e3) std::snprintf(b, sizeof b, "%.1f kb", bp / 1e3);
    else std::snprintf(b, sizeof b, "%.0f bp", bp);
    return b;
}
std::string pct(double f) {
    char b[16];
    std::snprintf(b, sizeof b, "%.1f%%", f * 100.0);
    return b;
}
// 1px lines 6px apart at 45 degrees, 70% opacity, clipped to the rect; aligned to a global grid so
// neighbouring hatches line up.
void hatch(ImDrawList* dl, float x0, float y0, float x1, float y1, float alpha = 0.7f) {
    if (x1 - x0 < 1.f) x1 = x0 + 1.f;
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    float h = y1 - y0;
    float start = std::floor((x0 - h - y0) / 6.f) * 6.f + y0;
    for (float x = start; x < x1 + 6.f; x += 6.f) dl->AddLine(ImVec2(x, y1), ImVec2(x + h, y0), col(kColNotIn, alpha), 1.f);
    dl->PopClipRect();
}
unsigned catColor(int c) {
    switch (c) {
        case kExon: return kColKnown;
        case kCcre: return kColCcre;
        case kIntron: return kColIntron;
        case kRepeat: return kColRepeat;
        default: return kColFog;
    }
}
unsigned baseCol(char b) {
    switch (std::toupper((unsigned char)b)) {
        case 'A': return 0x5CC48A;
        case 'C': return 0x6AA8FF;
        case 'G': return 0xF2B544;
        case 'T': return 0xF07178;
        default: return 0x8B93A1;
    }
}
const char* pillName(int c) {
    switch (c) {
        case kExon: return "Exon";
        case kCcre: return "Regulatory";
        case kIntron: return "Intron";
        case kRepeat: return "Repeat";
        default: return "Nothing known";
    }
}

// ---- track drawing primitives, shared by the region view and the legend swatches ----
constexpr unsigned kSvIns = 0x5B8DEF, kSvDel = 0xE8A13A, kSvCx = 0x8A8F99, kSvInvEdge = 0xB07CFF;
unsigned svColor(SvType t) { return t == SvType::Ins ? kSvIns : t == SvType::Del ? kSvDel : kSvCx; }
// SV bar: complex (grey, 1 px violet top edge when it contains an inversion) or deletion (amber); rare (AF < 1%) at 35%
void svBar(ImDrawList* dl, SvType t, bool inv, float af, float x0, float x1, float y0, float y1, bool outline, float mul,
           float clip0, float clip1) {
    float a = (af >= 0.01f ? 1.f : 0.35f) * mul;
    x0 = std::max(clip0, x0); x1 = std::min(clip1, x1);
    if (x1 <= x0) return;
    if (outline) dl->AddRect(ImVec2(x0 - 1, y0 - 1), ImVec2(x1 + 1, y1 + 1), col(kBg), 0, 0, 1.f);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col(svColor(t), a), 1);
    if (inv) dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + 1), col(kSvInvEdge, std::max(a, 0.6f)));
}
// SV insertion: 2 px tick, caret above (collapsed) or small notch (expanded rows)
void svIns(ImDrawList* dl, float x, float af, float y0, float y1, bool caret) {
    float a = af >= 0.01f ? 1.f : 0.35f;
    dl->AddRectFilled(ImVec2(x - 1, y0), ImVec2(x + 1, y1), col(kSvIns, a));
    if (caret) dl->AddTriangleFilled(ImVec2(x - 4, y0 - 5), ImVec2(x + 4, y0 - 5), ImVec2(x, y0), col(kSvIns, a));
    else dl->AddTriangleFilled(ImVec2(x - 3, y0), ImVec2(x + 3, y0), ImVec2(x, y0 + 3), col(kSvIns, a));
}
// small variants: accent ticks, rare (AF < 1%) dimmed to 45%
float variantAlpha(float af) { return af >= 0.01f ? 1.f : 0.45f; }
void variantTick(ImDrawList* dl, float x0, float x1, float y0, float y1, float af) {
    float a = variantAlpha(af);
    if (x1 - x0 > 2) dl->AddRectFilled(ImVec2(x0 + 1, y0), ImVec2(x1 - 1, y1), col(kAccent, a * 0.6f));
    else dl->AddLine(ImVec2(x0 + 0.5f, y0), ImVec2(x0 + 0.5f, y1), col(kAccent, a));
}
// CpG methylation ramp #2A2F3A (unmethylated) -> #C9A6FF (methylated)
constexpr unsigned kMethylLo = 0x2A2F3A, kMethylHi = 0xC9A6FF;
ImU32 methylCol(float t) {
    ImVec4 lo = vec(kMethylLo), hi = vec(kMethylHi);
    t = std::clamp(t, 0.f, 1.f);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(lo.x + (hi.x - lo.x) * t, lo.y + (hi.y - lo.y) * t, lo.z + (hi.z - lo.z) * t, 1.f));
}
// not-in-GRCh38 stretch: 18% purple wash + hatching
void notInFill(ImDrawList* dl, float x0, float y0, float x1, float y1) { dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col(kColNotIn, 0.18f)); }
// gene model: intron band (4 px, centred at bandY) and exon block (10 px)
void geneBand(ImDrawList* dl, float x0, float x1, float bandY) { dl->AddRectFilled(ImVec2(x0, bandY - 2), ImVec2(x1, bandY + 2), col(kColIntron)); }
void strandChevron(ImDrawList* dl, float cx, float bandY, float dir) {
    dl->AddLine(ImVec2(cx - 3 * dir, bandY - 3), ImVec2(cx + 1 * dir, bandY), col(kMuted));
    dl->AddLine(ImVec2(cx + 1 * dir, bandY), ImVec2(cx - 3 * dir, bandY + 3), col(kMuted));
}
void exonBlock(ImDrawList* dl, float x0, float x1, float y0, float y1) { dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col(kColKnown)); }
// pinned position marker and hover cursor
void pinMarker(ImDrawList* dl, float x, float top, float bot) {
    dl->AddLine(ImVec2(x, top - 8), ImVec2(x, bot + 4), col(kAccent), 1.5f);
    dl->AddTriangleFilled(ImVec2(x - 5, top - 12), ImVec2(x + 5, top - 12), ImVec2(x, top - 5), col(kAccent));
}
void hoverCursor(ImDrawList* dl, float x, float y0, float y1) { dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), col(kAccent, 0.6f)); }

}  // namespace

bool App::init(const AppOptions& opt, std::string& err) {
    opt_ = opt;
    loadPacks();
    if (opt.phone) {
        std::error_code ec;
        if (!std::filesystem::exists(opt.dataDir + "/tracks/overview.wov", ec)) {  // first run on a phone: download screen
            needData_ = true;
            loadSettings();
            introDone_ = true;
            return true;
        }
    }
    return initData(err);
}

bool App::initData(std::string& err) {
    const AppOptions& opt = opt_;
    needData_ = false;
    svExpanded_ = opt.expandSv;
    cardSvExpanded_ = opt.expandCard;
    std::string tracks = opt.dataDir + "/tracks";
    if (!ov_.load(tracks + "/overview.wov", err)) return false;
    std::string vcf = opt.variants;
    if (vcf.empty()) {
        std::error_code ec;
        std::vector<std::string> found;
        for (auto& e : std::filesystem::directory_iterator(opt.dataDir, ec)) {
            std::string pth = e.path().string();
            if (pth.size() > 7 && pth.compare(pth.size() - 7, 7, ".vcf.gz") == 0 && std::filesystem::exists(pth + ".tbi", ec))
                found.push_back(pth);
        }
        std::sort(found.begin(), found.end());
        if (!found.empty()) vcf = found.front();
    }
    if (!store_.open(tracks, vcf, err)) return false;
    std::fprintf(stderr, "variants: %s\n", store_.hasVariants() ? vcf.c_str() : "(none)");
    {  // genome sequence is optional (a 933 MB download on phones): without it the Sequence lane stays empty
        std::string gerr;
        if (!genome_.open(opt.dataDir + "/hs1.fa.bgz", gerr)) std::fprintf(stderr, "genome: %s (sequence lane disabled)\n", gerr.c_str());
    }
    {
        std::string ge;
        std::string gp = tracks + "/genes.idx.tsv.gz";
        std::error_code ec2;
        auto sz = std::filesystem::file_size(gp, ec2);
        if (genes_.load(gp, ge))
            std::fprintf(stderr, "gene index: %zu records, %zu aliases, %zu extra copies (%.0f KB)\n", genes_.size(),
                         genes_.aliasCount(), genes_.extraCount(), ec2 ? 0.0 : double(sz) / 1024.0);
        else
            std::fprintf(stderr, "gene index: %s (run scripts/prepare_tracks.sh)\n", ge.c_str());
    }
    // natural order chr1..chr22, chrX, chrY (chrM is too small to show)
    auto rank = [](const std::string& n) {
        std::string s = n.rfind("chr", 0) == 0 ? n.substr(3) : n;
        if (s == "X") return 23;
        if (s == "Y") return 24;
        if (s == "M") return 99;
        return std::isdigit((unsigned char)s[0]) ? std::atoi(s.c_str()) : 50;
    };
    for (int i = 0; i < int(ov_.chroms().size()); ++i)
        if (rank(ov_.chroms()[i].name) < 99) order_.push_back(i);
    std::sort(order_.begin(), order_.end(), [&](int a, int b) { return rank(ov_.chroms()[a].name) < rank(ov_.chroms()[b].name); });
    {
        std::vector<float> kn, no;
        for (int ci = 0; ci < int(ov_.chroms().size()); ++ci)
            for (uint32_t b = 0; b < ov_.chroms()[ci].nbins; ++b) {
                // percentiles over non-zero bins only (zero bins get alpha 0 anyway)
                float k = ov_.frac(ci, b, kExon) + ov_.frac(ci, b, kCcre), z = ov_.frac(ci, b, kNothing);
                if (k > 0.f) kn.push_back(k);
                if (z > 0.f) no.push_back(z);
            }
        auto q = [](std::vector<float> v, double p) {
            std::sort(v.begin(), v.end());
            return v.empty() ? 0.f : v[size_t(p * double(v.size() - 1) + 0.5)];
        };
        knownP5_ = q(kn, 0.05); knownP95_ = q(kn, 0.95);
        nothingP5_ = q(no, 0.05); nothingP95_ = q(no, 0.95);
        std::fprintf(stderr, "overview stretch: known p5=%.4f p95=%.4f, nothing p5=%.4f p95=%.4f (non-zero bins: %zu known, %zu nothing)\n",
                     knownP5_, knownP95_, nothingP5_, nothingP95_, kn.size(), no.size());
    }
    setView(order_.empty() ? 0 : order_[0], 0, 1e18);
    if (!opt.region.empty()) {
        int ci;
        double s, e;
        if (!parseLocus(opt.region, ci, s, e)) { err = "bad --region " + opt.region; return false; }
        setView(ci, s, e);
    }
    if (!opt.pin.empty()) {
        int ci;
        double s, e;
        if (!parseLocus(opt.pin, ci, s, e)) { err = "bad --pin " + opt.pin; return false; }
        pinned_ = {true, ci, int64_t(s), false};
    }
    {
        std::string rp = opt.rsidPack.empty() ? opt.dataDir + "/packs/rsid/rsid.idx" : opt.rsidPack;
        if (rsid_.open(rp)) std::fprintf(stderr, "rsID pack: %u rsIDs (%s)\n", rsid_.count(), rp.c_str());
        else std::fprintf(stderr, "rsID pack: not installed (%s)\n", rp.c_str());
        packCard_ = opt.packCard;
        if (opt.packDownload) startPackDownload();
    }
    loadSettings();
    if (opt.phone) { introDone_ = true; legendOpen_ = false; }  // the desktop intro/legend layout does not apply
    if (opt.legend) legendOpen_ = true;
    if (opt.phone) {
        sheetState_ = opt.phoneSheet == "half" ? 1 : opt.phoneSheet == "full" ? 2 : 0;
        phoneDownloads_ = opt.phoneDownloads;
        if (!opt.search.empty()) phoneSearch_ = true;
    }
    // first run: data is present (init got this far) and no settings flag yet
    if (opt.intro) startIntro();
    else if (!introDone_) { if (packCard_) introPending_ = true; else startIntro(); }
    if (!opt.search.empty()) {
        std::snprintf(searchBuf_, sizeof searchBuf_, "%s", opt.search.c_str());
        searchOpen_ = searchFocus_ = true;
        searchMore_ = opt.searchMore;
        if (!opt.searchGroup.empty()) {  // shots: open the first extra-copy group of this symbol, scrolled 3 rows into its copies
            for (auto& h : genes_.search(opt.search, 2000))
                if (!h.g->extra && h.g->name == opt.searchGroup) { openGroup_ = h.g->name + "@" + h.g->chrom + std::to_string(h.g->s); break; }
            searchMore_ = true;
            scrollToGroup_ = 4;
        }
    }
    if (!opt.gotoQuery.empty() && !runQuery(opt.gotoQuery)) { err = "no match for --goto " + opt.gotoQuery; return false; }
    return true;
}

bool App::parseLocus(const std::string& txt, int& ci, double& s, double& e) const {
    std::string t;
    for (char c : txt)
        if (c != ',' && c != ' ') t.push_back(c);
    auto colon = t.find(':');
    std::string name = t.substr(0, colon);
    ci = ov_.find(name);
    if (ci < 0) ci = ov_.find("chr" + name);
    if (ci < 0) return false;
    if (colon == std::string::npos) { s = 0; e = double(ov_.chroms()[ci].length); return true; }
    std::string r = t.substr(colon + 1);
    auto dash = r.find('-');
    s = std::atof(r.substr(0, dash).c_str()) - 1;  // 1-based input
    e = dash == std::string::npos ? s + 1 : std::atof(r.substr(dash + 1).c_str());
    return e > s;
}

void App::setView(int ci, double s, double e) {
    const double len = double(ov_.chroms()[ci].length);
    double minSpan = std::max(20.0, std::floor(regionWidthPx_ / 16.0));
    double span = std::clamp(e - s, minSpan, len);
    double c = (s + e) * 0.5;
    if (e - s < minSpan) s = c - span * 0.5;
    s = std::clamp(s, 0.0, len - span);
    viewCi_ = ci;
    viewS_ = s;
    viewE_ = s + span;
}

float App::stretchAlpha(float share, float p5, float p95) const {
    if (share <= 0.f) return 0.f;
    float t = p95 > p5 ? std::clamp((share - p5) / (p95 - p5), 0.f, 1.f) : 1.f;
    t = t * t;  // gamma 2
    return 0.1f + (0.85f - 0.1f) * t;
}

bool App::idle() const {
    bool introFading = introStep_ >= 0 && !opt_.reducedMotion && clock_ - introT0_ < 0.2;
    return store_.pending() == 0 && dlState_ != 1 && !animating_ && clock_ - hlT0_ > 0.75 && !introFading;  // shots catch the outline mid-fade
}  // shots catch the outline mid-fade

void App::gather(int ci, int64_t s, int64_t e, Visible& v) {
    const std::string& chrom = ov_.chroms()[ci].name;
    std::set<std::tuple<int64_t, int64_t, std::string>> seenGenes;
    for (int64_t t = s / TrackStore::kTile; t <= (e - 1) / TrackStore::kTile; ++t) {
        for (int k = 0; k < kTrackCount; ++k) {
            if (k == int(Track::Variants) && !store_.hasVariants()) continue;
            if (k == int(Track::Sv) && !store_.hasSv()) continue;
            if (k == int(Track::Methyl) && !store_.hasMethyl()) continue;
            TilePtr tile = store_.request(Track(k), chrom, t);
            if (!tile) { v.loading[k] = true; continue; }
            v.hold.push_back(tile);
            for (auto& g : tile->genes)
                if (g.e > s && g.s < e && seenGenes.insert({g.s, g.e, g.name}).second) v.genes.push_back(&g);
            for (auto& x : tile->exons) if (x.e > s && x.s < e) v.exons.push_back(&x);
            for (auto& x : tile->ccres) if (x.e > s && x.s < e) v.ccres.push_back(&x);
            for (auto& x : tile->repeats) if (x.e > s && x.s < e) v.repeats.push_back(&x);
            for (auto& x : tile->notin) if (x.e > s && x.s < e) v.notin.push_back(&x);
            for (auto& x : tile->variants) if (x.e > s && x.s < e) v.variants.push_back(&x);
            // SV groups: take each group from the tile its start falls in (or the first tile for groups starting
            // before the view), so groups spanning tile borders are not duplicated
            int64_t t0 = s / TrackStore::kTile;
            for (auto& g : tile->svGroups) {
                if (!(g.e > s && g.s < e)) continue;
                if (!(g.s / TrackStore::kTile == t || (t == t0 && g.s < t0 * TrackStore::kTile))) continue;
                v.svGroups.push_back(&g);
                for (int i = 0; i < g.count; ++i) v.svs.push_back(&g.alleles[i]);
            }
            for (auto& x : tile->methyl) if (int64_t(x.e) > s && int64_t(x.s) < e) v.methyl.push_back(&x);
        }
    }
}

void App::frame(FogRenderer& fog) {
    if (opt_.phone) { framePhone(fog); return; }
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 D = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(D);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    hover_ = {};
    if (virtualClock_ >= 0) clock_ = virtualClock_;
    else clock_ += io.DeltaTime;
    if (animating_) {  // 300 ms ease-out pan+zoom
        double t = std::clamp((clock_ - animT0_) / 0.3, 0.0, 1.0);
        double k = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
        double s0 = animS0_ + (animS1_ - animS0_) * k, e0 = animE0_ + (animE1_ - animE0_) * k;
        if (t >= 1.0) { s0 = animS1_; e0 = animE1_; animating_ = false; }
        setView(viewCi_, s0, e0);
    }
    // first-run intro after the pack download finishes (once its card is closed)
    {
        int st = dlState_;
        if (lastDlState_ == 1 && st == 2 && !introDone_) introPending_ = true;
        lastDlState_ = st;
        if (introPending_ && !packCard_) { introPending_ = false; startIntro(); }
    }
    const bool introOpen = introStep_ >= 0;
    if (!introOpen && !packCard_ && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_L, false)) {
        legendOpen_ = !legendOpen_;
        saveSettings();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (introOpen) endIntro();  // Esc skips the intro
        else if (packCard_) {}  // the card closes itself
        else if (searchOpen_) searchOpen_ = false, ImGui::SetWindowFocus(nullptr);
        else pinned_ = {};
    }

    // modal open: the map gets no hover or clicks
    ImVec2 realMouse = io.MousePos;
    if (packCard_ || introOpen) io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
    drawTopBar(dl, D.x);
    float y = drawCoverage(dl, kTopBarH, D.x);
    float leftW = D.x - kMargin * 3 - kCardW;
    ImVec2 ovA(kMargin, y), ovB(kMargin + leftW, y + kOverviewH);
    ImVec2 rgA(kMargin, ovB.y + 12), rgB(kMargin + leftW, D.y - kMargin);
    ImVec2 cdA(D.x - kMargin - kCardW, y), cdB(D.x - kMargin, D.y - kMargin);
    const float kLegendW = 280.f, kLegendGap = 12.f;
    ImVec2 lgA(rgB.x - kLegendW, rgA.y), lgB = rgB;
    if (legendOpen_) rgB.x -= kLegendW + kLegendGap;  // the legend takes the right 280 px of the region view
    ovA_ = ovA; ovB_ = ovB; rgA_ = rgA; rgB_ = rgB;
    drawOverview(dl, ovA, ovB, fog);
    drawRegion(dl, rgA, rgB, fog);
    if (legendOpen_) drawLegend(lgA, lgB, fog);
    drawCard(dl, cdA, cdB);
    if (!introOpen) io.MousePos = realMouse;
    drawSearch(D.x);
    drawTopButtons(D.x);
    io.MousePos = realMouse;
    drawPackCard(D);
    drawIntro(D);
    ImGui::End();
}

void App::jumpTo(int ci, double s, double e) {
    if (ci != viewCi_) { setView(ci, s, e); animating_ = false; return; }  // other chromosome: cut, no pan
    animS0_ = viewS_; animE0_ = viewE_;
    // final view after clamping, so the animation lands exactly
    double cs = viewS_, ce = viewE_;
    setView(ci, s, e);
    animS1_ = viewS_; animE1_ = viewE_;
    setView(ci, cs, ce);
    animT0_ = clock_;
    animating_ = true;
}

bool App::runQuery(const std::string& q) {
    int ci;
    double s, e;
    if (q.find(':') != std::string::npos && parseLocus(q, ci, s, e)) {
        if (e - s <= 1) { s -= 50; e += 50; }  // single position: 100 bp around it
        jumpTo(ci, s, e);
        return true;
    }
    if (q.size() > 2 && (q[0] == 'r' || q[0] == 'R') && (q[1] == 's' || q[1] == 'S') &&
        q.find_first_not_of("0123456789", 2) == std::string::npos && q.size() < 13) {
        RsidHit h;
        unsigned long long n = std::stoull(q.substr(2));
        if (n >= 4294967296ull || !rsid_.find(uint32_t(n), h) || (ci = ov_.find(h.chrom)) < 0) return false;
        double c = double(h.pos) - 0.5;
        jumpTo(ci, c - 100, c + 100);  // ~200 bp of context
        pinned_ = {true, ci, h.pos - 1, false};
        lastRs_ = h;
        return true;
    }
    auto hits = genes_.search(q, 8);
    if (hits.empty()) return false;
    const GeneEntry* g = hits[std::min<size_t>(size_t(std::max(0, searchSel_)), hits.size() - 1)].g;
    if (!searchOpen_) g = hits[0].g;
    ci = ov_.find(g->chrom);
    if (ci < 0) return false;
    double pad = double(g->e - g->s) * 0.10;
    jumpTo(ci, double(g->s) - pad, double(g->e) + pad);
    hlName_ = g->name; hlChrom_ = g->chrom; hlS_ = g->s; hlT0_ = clock_;
    return true;
}

void App::drawSearch(float w) {
    const bool phone = opt_.phone;  // phone: full-screen search, input in the top bar, 48 dp rows
    const float bw = phone ? w - 56 : 320, bh = phone ? 40 : 32;
    ImVec2 a = phone ? ImVec2(48, 4) : ImVec2((w - bw) * 0.5f, (kTopBarH - bh) * 0.5f);
    ImGuiIO& io = ImGui::GetIO();
    bool typing = io.WantTextInput;
    if (introStep_ < 0 && ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)) || (!typing && ImGui::IsKeyPressed(ImGuiKey_Slash)))) searchFocus_ = true;
    searchA_ = a; searchB_ = ImVec2(a.x + bw, a.y + bh);
    ImGui::SetCursorScreenPos(a);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, (bh - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, vec(kPanel));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, vec(kPanel));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, vec(kPanel));
    ImGui::SetNextItemWidth(bw);
    if (searchFocus_) { ImGui::SetKeyboardFocusHere(); searchFocus_ = false; }
    const char* ph = rsid_.present() ? "Gene, rsID or chr22:20,340,000" : "Gene or chr22:20,340,000";
    bool enter = ImGui::InputTextWithHint("##search", ph, searchBuf_, sizeof searchBuf_,
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    bool active = ImGui::IsItemActive();
    if (ImGui::IsItemEdited()) { searchOpen_ = true; searchSel_ = 0; searchScroll_ = 0; searchMore_ = false; openGroup_.clear(); }
    if (ImGui::IsItemActivated()) searchOpen_ = true;
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
    // root-window draw list (after the frame, so above it) - never the foreground list, which would sit above the intro scrim
    ImDrawList* fg = ImGui::GetWindowDrawList();
    fg->AddRect(a, ImVec2(a.x + bw, a.y + bh), col(active || searchOpen_ ? kAccent : kBorder), 6, 0, 1.f);

    std::string q = searchBuf_;
    bool hasText = q.find_first_not_of(' ') != std::string::npos;
    if (!searchOpen_ || !hasText) { dropA_ = dropB_ = ImVec2(-1, -1); return; }
    // rows: a coordinate row, gene hits, or the no-match row
    enum Kind { kGeneRow, kLocusRow, kMoreRow, kCopiesRow, kRsRow, kHintRow, kPackRow };
    struct Row { std::string label, sub, tag; const GeneEntry* g = nullptr; Kind kind = kGeneRow; int n = 0; RsidHit rs{}; };
    std::vector<Row> rows;
    int stickyIdx = -1, stickyEnd = -1;  // opened extra-copy group: main row index, one past its last copy
    const int cap = 8;
    const int maxVisible = phone ? std::max(4, int((ImGui::GetIO().DisplaySize.y - 64) / 48.f)) : 16;
    int ci; double s, e;
    if (q.find(':') != std::string::npos && parseLocus(q, ci, s, e)) {
        std::string t = ov_.chroms()[ci].name + ":" + commas(int64_t(s) + 1);
        if (e - s > 1) t += "-" + commas(int64_t(e));
        rows.push_back({"Go to", t, "", nullptr, kLocusRow});
    } else if (q.size() > 2 && (q[0] == 'r' || q[0] == 'R') && (q[1] == 's' || q[1] == 'S') &&
               q.find_first_not_of("0123456789 ", 2) == std::string::npos) {
        // rsID: only with the optional variant pack
        std::string num = q.substr(2);
        num.erase(std::remove(num.begin(), num.end(), ' '), num.end());
        RsidHit h;
        if (!rsid_.present()) {
            rows.push_back({"rsID search needs the optional variant pack, get it", "", "", nullptr, kPackRow});
        } else if (!num.empty() && num.size() < 11 && std::stoull(num) < 4294967296ull && rsid_.find(uint32_t(std::stoull(num)), h)) {
            Row r{"rs" + num, "", "", nullptr, kRsRow};
            r.sub = h.chrom + ":" + commas(h.pos) + " " + h.ref + ">" + h.alt;
            r.rs = h;
            rows.push_back(r);
        } else {
            rows.push_back({"rs" + num + " isn't in the common-variant pack. It may be rare, retired, or not mapped to hs1.", "", "", nullptr, kHintRow});
        }
    } else {
        auto hits = genes_.search(q, 2000);  // all matches (bounded); main copies already followed by their extra copies
        auto geneRow = [&](const GeneHit& h) {
            Row r{h.g->name, h.g->chrom + ":" + commas(h.g->s + 1), "", h.g};
            if (h.tier == 2) r.tag = "alias of " + h.g->name, r.label = h.alias;
            return r;
        };
        // split hits into groups: a main copy plus the extra copies that follow it (same symbol)
        std::vector<std::pair<size_t, size_t>> groups;
        for (size_t i = 0; i < hits.size();) {
            size_t j = i + 1;
            if (!hits[i].g->extra)
                while (j < hits.size() && hits[j].g->extra && hits[j].g->name == hits[i].g->name) ++j;
            groups.push_back({i, j});
            i = j;
        }
        auto keyOf = [](const GeneEntry* g) { return g->name + "@" + g->chrom + std::to_string(g->s); };
        size_t shown = 0;
        bool limited = !searchMore_ && hits.size() > size_t(cap);
        int budget = limited ? cap - 1 : INT32_MAX;  // last row becomes '+N more'
        for (auto [i, j] : groups) {
            if (budget <= 0) break;
            bool open = !openGroup_.empty() && keyOf(hits[i].g) == openGroup_;
            if (open) stickyIdx = int(rows.size());
            rows.push_back(geneRow(hits[i]));
            ++shown; --budget;
            size_t copies = j - i - 1;
            if (open) stickyEnd = int(rows.size() + copies);
            if (!copies) continue;
            if (int(copies) <= budget || !limited) {
                for (size_t k = i + 1; k < j; ++k) rows.push_back(geneRow(hits[k]));
                shown += copies; budget -= int(copies);
            } else if (budget > 0) {
                Row r{"+" + std::to_string(copies) + (copies == 1 ? " copy" : " copies"), "", "", hits[i].g, kCopiesRow, int(copies)};
                rows.push_back(r);
                shown += copies; --budget;  // counted as represented, not in '+N more'
            } else break;
        }
        if (limited && shown < hits.size()) {
            int n = int(hits.size() - shown);
            rows.push_back({"+" + std::to_string(n) + " more", "", "", nullptr, kMoreRow, n});
        }
    }
    const bool tallHint = rows.size() == 1 && (rows[0].kind == kHintRow || rows[0].kind == kPackRow);
    const float rh = phone ? (tallHint ? 72 : 48) : tallHint ? 48 : 28;  // wrapped two-line message; 48 dp touch rows
    int visible = std::min<int>(int(rows.size()), maxVisible);
    if (!rows.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) searchSel_ = std::min(int(rows.size()) - 1, searchSel_ + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) searchSel_ = std::max(0, searchSel_ - 1);
        searchSel_ = std::clamp(searchSel_, 0, int(rows.size()) - 1);
    }
    float nr = float(std::max(1, visible));
    ImVec2 d0(a.x, a.y + bh + 4), d1(a.x + bw, a.y + bh + 4 + nr * rh + 8);
    if (phone) { d0 = ImVec2(0, 52); d1 = ImVec2(w, 52 + nr * rh + 8); }
    dropA_ = d0; dropB_ = d1;
    int maxScroll = std::max(0, int(rows.size()) - visible);
    if (ImGui::IsMouseHoveringRect(d0, d1, false) && io.MouseWheel != 0) searchScroll_ -= int(io.MouseWheel * 3);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {  // keep selection in view
        if (searchSel_ < searchScroll_) searchScroll_ = searchSel_;
        if (searchSel_ >= searchScroll_ + visible) searchScroll_ = searchSel_ - visible + 1;
    }
    if (stickyIdx >= 0 && scrollToGroup_) { searchScroll_ = stickyIdx + scrollToGroup_ - 1; scrollToGroup_ = 0; }
    searchScroll_ = std::clamp(searchScroll_, 0, maxScroll);
    // sticky header: the opened group's main row stays on top while its copies scroll under it
    bool sticky = stickyIdx >= 0 && searchScroll_ > stickyIdx && searchScroll_ < stickyEnd;
    fg->AddRectFilled(d0, d1, col(kPanel), 6);
    fg->AddRect(d0, d1, col(kBorder), 6);
    int clicked = -1;
    if (rows.empty()) {
        std::string t = "No gene called " + q + " in hs1";
        fg->AddText(fonts().ui, 15, ImVec2(d0.x + 12, d0.y + 4 + (rh - 15) * 0.5f), col(0x8A8F99), t.c_str());
    }
    fg->PushClipRect(d0, d1, true);
    std::vector<int> order;  // row index per visible slot
    if (sticky) order.push_back(stickyIdx);
    for (int i = searchScroll_ + (sticky ? 1 : 0); i < int(rows.size()) && int(order.size()) < visible; ++i) order.push_back(i);
    for (int slot = 0; slot < int(order.size()); ++slot) {
        int i = order[size_t(slot)];
        const Row& rw = rows[size_t(i)];
        ImVec2 r0(d0.x + 4, d0.y + 4 + slot * rh), r1(d1.x - 4, r0.y + rh);
        if (sticky && slot == 0) {  // header background + divider so copies visibly pass beneath
            fg->AddRectFilled(ImVec2(d0.x + 1, r0.y - 3), ImVec2(d1.x - 1, r1.y), col(kPanel));
            fg->AddLine(ImVec2(d0.x + 1, r1.y), ImVec2(d1.x - 1, r1.y), col(kBorder));
        }
        bool hov = ImGui::IsMouseHoveringRect(r0, r1, false);
        if (hov && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) searchSel_ = i;
        if (hov && ImGui::IsMouseClicked(0)) clicked = i;
        if (i == searchSel_) fg->AddRectFilled(r0, r1, col(kHover), 4);
        float ty = r0.y + (rh - 15) * 0.5f;
        if (rw.kind == kHintRow || rw.kind == kPackRow) {
            fg->AddText(fonts().ui, 15, ImVec2(r0.x + 8, r0.y + 4), col(0x8A8F99), rw.label.c_str(), nullptr, r1.x - r0.x - 16);
            if (rw.kind == kPackRow) {  // underline 'get it' as the affordance
                float full = fonts().ui->CalcTextSizeA(15, FLT_MAX, 0, rw.label.c_str()).x;
                float tail = fonts().ui->CalcTextSizeA(15, FLT_MAX, 0, "get it").x;
                if (full < r1.x - r0.x - 16)
                    fg->AddLine(ImVec2(r0.x + 8 + full - tail, r0.y + 21), ImVec2(r0.x + 8 + full, r0.y + 21), col(0x8A8F99));
            }
            continue;
        }
        if (rw.kind == kRsRow) {
            float ty2 = r0.y + (rh - 15) * 0.5f;
            fg->AddText(fonts().ui, 15, ImVec2(r0.x + 8, ty2 - 1), col(kText), rw.label.c_str());
            float lw2 = fonts().ui->CalcTextSizeA(15, FLT_MAX, 0, rw.label.c_str()).x;
            fg->AddText(fonts().ui, 15, ImVec2(r0.x + 8 + lw2 + 6, ty2 - 1), col(0x8A8F99), "\xC2\xB7");
            fg->AddText(fonts().mono, 13, ImVec2(r0.x + 8 + lw2 + 18, ty2 + 1), col(0x8A8F99), rw.sub.c_str());
            continue;
        }
        if (rw.kind == kMoreRow || rw.kind == kCopiesRow) {
            fg->AddText(fonts().ui, 15, ImVec2(r0.x + 8 + (rw.kind == kCopiesRow ? 16 : 0), ty - 1), col(0x8A8F99), rw.label.c_str());
            continue;
        }
        fg->AddText(fonts().ui, 15, ImVec2(r0.x + 8, ty - 1), col(kText), rw.label.c_str());
        float lw = fonts().ui->CalcTextSizeA(15, FLT_MAX, 0, rw.label.c_str()).x;
        fg->AddText(fonts().mono, 13, ImVec2(r0.x + 8 + lw + 10, ty + 1), col(0x8A8F99), rw.sub.c_str());
        float px = r1.x - 4;
        auto pill = [&](const std::string& t, bool extra) {
            const float pfs = opt_.phone ? 12.f : 11.f;
            float tw = fonts().monoSmall->CalcTextSizeA(pfs, FLT_MAX, 0, t.c_str()).x;
            ImVec2 p0(px - tw - 10, r0.y + (rh - 16) * 0.5f), p1(px, r0.y + (rh - 16) * 0.5f + 16);  // 16 tall, centred in the row
            if (extra) {
                fg->AddRectFilled(p0, p1, col(0x8A8F99, 0.18f), 8);
                fg->AddRect(p0, p1, col(0x8A8F99), 8);
                fg->AddText(fonts().monoSmall, pfs, ImVec2(p0.x + 5, p0.y + 1), col(0x8A8F99), t.c_str());
            } else {
                fg->AddRectFilled(p0, p1, col(kBorder), 8);
                fg->AddText(fonts().monoSmall, pfs, ImVec2(p0.x + 5, p0.y + 1), col(kMuted), t.c_str());
            }
            px = p0.x - 4;
        };
        if (rw.g && rw.g->extra) pill("extra copy", true);
        if (!rw.tag.empty()) pill(rw.tag, false);
    }
    if (maxScroll > 0) {  // scrollbar
        float h = d1.y - d0.y - 8, th = h * float(visible) / float(rows.size());
        float ty = d0.y + 4 + (h - th) * float(searchScroll_) / float(maxScroll);
        fg->AddRectFilled(ImVec2(d1.x - 5, ty), ImVec2(d1.x - 2, ty + th), col(kBorder), 2);
    }
    fg->PopClipRect();
    if (clicked >= 0) { searchSel_ = clicked; enter = true; }
    if (enter && !rows.empty()) {
        const Row& r = rows[size_t(searchSel_)];
        if (r.kind == kHintRow) {
            searchFocus_ = true;  // informational only
        } else if (r.kind == kPackRow) {
            packCard_ = true;
            searchOpen_ = false;
        } else if (r.kind == kRsRow) {
            int rci = ov_.find(r.rs.chrom);
            if (rci >= 0) {
                double c = double(r.rs.pos) - 0.5;
                jumpTo(rci, c - 100, c + 100);  // ~200 bp of context
                pinned_ = {true, rci, r.rs.pos - 1, false};
                lastRs_ = r.rs;
            }
            searchOpen_ = false;
            ImGui::SetWindowFocus(nullptr);
        } else if (r.kind == kMoreRow) {
            searchMore_ = true;
            searchFocus_ = true;  // keep typing focus, list stays open
        } else if (r.kind == kCopiesRow) {
            // one expanded state: switch to the 16-row scrolling list with this group opened, main row on top
            openGroup_ = r.g->name + "@" + r.g->chrom + std::to_string(r.g->s);
            searchMore_ = true;
            scrollToGroup_ = 1;
            searchFocus_ = true;
        } else {
            if (r.kind == kLocusRow) runQuery(q);
            else {
                int gci = ov_.find(r.g->chrom);
                if (gci >= 0) {
                    double pad = double(r.g->e - r.g->s) * 0.10;
                    jumpTo(gci, double(r.g->s) - pad, double(r.g->e) + pad);
                    hlName_ = r.g->name; hlChrom_ = r.g->chrom; hlS_ = r.g->s; hlT0_ = clock_;
                }
            }
            searchOpen_ = false;
            ImGui::SetWindowFocus(nullptr);
        }
    } else if (!phone && !active && ImGui::IsMouseClicked(0) && !ImGui::IsMouseHoveringRect(d0, d1, false)) {
        searchOpen_ = false;
    }
    if (phone && !searchOpen_) phoneSearch_ = false;  // a result was chosen: back to the map
    if (!searchOpen_) dropA_ = dropB_ = ImVec2(-1, -1);
}

std::string App::rsidSizeStr() const { return kRsidPackSize; }

App::~App() {
    if (dlThread_.joinable()) dlThread_.detach();  // the build script keeps running; its output lands in data/packs
}

static std::map<std::string, std::string> readConf(const std::string& path) {
    std::map<std::string, std::string> m;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return m;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        auto eq = l.find('=');
        if (l.empty() || l[0] == '#' || eq == std::string::npos) continue;
        m[l.substr(0, eq)] = l.substr(eq + 1);
    }
    std::fclose(f);
    return m;
}

void App::loadPacks() {
    packs_.clear();
    FILE* f = std::fopen((opt_.assetsDir + "/data_packs.tsv").c_str(), "r");
    if (!f) return;
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char pack[64], asset[256], path[512], sha[80];
        long long bytes = 0;
        if (std::sscanf(line, "%63s %255s %511s %lld %79s", pack, asset, path, &bytes, sha) == 5) packs_.push_back({pack, asset, path, bytes, sha});
    }
    std::fclose(f);
}

std::string App::packDest(const PackFile& f) const {
    if (f.pack == "rsid" && !opt_.rsidPack.empty()) return opt_.rsidPack;
    return opt_.dataDir + "/" + f.path;
}

long long App::packBytes(const std::string& pack) const {
    long long t = 0;
    for (auto& f : packs_) if (f.pack == pack) t += f.bytes;
    return t;
}

bool App::packInstalled(const std::string& pack) const {
    bool any = false;
    std::error_code ec;
    for (auto& f : packs_) {
        if (f.pack != pack) continue;
        any = true;
        if ((long long)std::filesystem::file_size(packDest(f), ec) != f.bytes || ec) return false;
    }
    return any;
}

void App::startPackDownload() { startDownload("rsid"); }

// Downloads every file of a pack from the GitHub data release (assets/data_release.conf DATA_BASE_URL, or
// $WISP_DATA_BASE_URL for testing) into the data root, SHA-256 checked in-process (util/sha256), .part then rename.
void App::startDownload(const std::string& pack) {
    if (dlState_ == 1) return;
    namespace fs = std::filesystem;
    auto conf = readConf(opt_.assetsDir + "/data_release.conf");
    std::string base = std::getenv("WISP_DATA_BASE_URL") ? std::getenv("WISP_DATA_BASE_URL") : conf["DATA_BASE_URL"];  // env: testing
    std::vector<PackFile> files;
    for (auto& f : packs_) if (f.pack == pack) files.push_back(f);
    if (base.empty() || files.empty()) {
        std::lock_guard<std::mutex> l(dlMu_);
        dlMsg_ = base.empty() ? "assets/data_release.conf missing" : "assets/data_packs.tsv has no '" + pack + "' files";
        dlPack_ = pack;
        dlState_ = 3;
        return;
    }
    long long total = 0;
    for (auto& f : files) total += f.bytes;
    dlPack_ = pack;
    dlState_ = 1; dlDoneBytes_ = 0; dlCurBytes_ = 0; dlTotal_ = total; dlPhase_ = 0; dlT0_ = clock_;
    if (dlThread_.joinable()) dlThread_.join();
    std::vector<std::pair<PackFile, std::string>> jobs;
    for (auto& f : files) jobs.push_back({f, packDest(f)});
    dlThread_ = std::thread([this, base, jobs] {
        auto fail = [&](const std::string& m) {
            std::lock_guard<std::mutex> l(dlMu_);
            dlMsg_ = m;
            dlState_ = 3;
        };
        for (auto& [f, dest] : jobs) {
            std::error_code ec;
            if ((long long)fs::file_size(dest, ec) == f.bytes && !ec) { dlDoneBytes_ += f.bytes; continue; }  // already there
            fs::create_directories(fs::path(dest).parent_path(), ec);
            std::string part = dest + ".part";
            dlPhase_ = 0;
            dlCurBytes_ = 0;
            DownloadResult r = downloadFile(base + "/" + f.asset, part, "ProjectWisp: " + f.asset, &dlCurBytes_);
            if (!r.ok) { fs::remove(part, ec); return fail(f.asset + ": " + r.err); }
            if (r.sha256 != f.sha) { fs::remove(part, ec); return fail(f.asset + ": checksum mismatch (got " + r.sha256.substr(0, 12) + "...)"); }
            fs::rename(part, dest, ec);
            if (ec) return fail("could not move " + f.asset + " into place");
            dlDoneBytes_ += f.bytes;
            dlCurBytes_ = 0;
        }
        dlState_ = 2;
    });
}

void App::drawPackCard(ImVec2 D) {
    // reopen the pack once a background build finished
    if (dlState_ == 2 && !rsid_.present()) {
        std::string rp = opt_.rsidPack.empty() ? opt_.dataDir + "/packs/rsid/rsid.idx" : opt_.rsidPack;
        rsid_.open(rp);
    }
    if (!packCard_) return;
    // first-run style download card, showing only the optional rsID pack
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), D, col(0x0F1115, 0.60f));  // 60% scrim over the root window
    searchOpen_ = false;
    const bool phone = opt_.phone;
    const float w = phone ? D.x - 24 : 480, bh = phone ? 44 : 30;  // 44 dp touch targets on phones
    ImGui::SetNextWindowPos(phone ? ImVec2(12, D.y * 0.16f) : ImVec2((D.x - w) * 0.5f, D.y * 0.28f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(kPanel));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(kBorder));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, vec(kAccent));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, vec(kHover));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    ImGui::Begin("##packcard", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(fonts().monoSmall);
    ImGui::TextColored(vec(kMuted), "OPTIONAL DOWNLOAD");
    ImGui::PopFont();
    ImGui::PushFont(fonts().uiBold);
    ImGui::Text("Common variants (rsID search)");
    ImGui::PopFont();
    ImGui::SameLine(w - 16 - ImGui::CalcTextSize((std::string(kRsidPackSize) + " download").c_str()).x - 24);
    ImGui::PushFont(fonts().mono);
    ImGui::TextColored(vec(kText), "%s download", kRsidPackSize);
    ImGui::PopFont();
    if (phone) {  // app-specific storage: the size warning follows the total size
        ImGui::PushFont(fonts().ui13);
        ImGui::TextColored(vec(0x8A8F99), "%s", kStorageNote);
        ImGui::PopFont();
    }
    ImGui::PushTextWrapPos(w - 16);
    ImGui::TextColored(vec(kMuted), "%s rsIDs from dbSNP 155 on T2T-CHM13 (UCSC), common variants only (minor allele >= 1%% in any "
                                    "1000 Genomes population).",
                       kRsidPackCount);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    int st = dlState_;
    if (rsid_.present()) {
        ImGui::TextColored(vec(kAccent), "Installed. Type an rs number in the search box.");
    } else if (st == 1) {
        long long b = dlDoneBytes_ + dlCurBytes_, t = dlTotal_;
        int ph = dlPhase_;
        if (virtualClock_ >= 0) {  // script mode: progress from the virtual clock (20 MB/s), not the wall-time file size
            b = std::min<long long>(t, (long long)((clock_ - dlT0_) * 20000.0 * 1024.0));
            ph = b >= t ? 1 : 0;
        }
        float frac = ph == 0 ? (t > 0 ? std::min(1.f, float(double(b) / double(t))) : 0.f) : 1.f;
        char over[96];
        if (ph == 0) std::snprintf(over, sizeof over, "Downloading %.0f / %.0f MB", b / 1048576.0, t / 1048576.0);
        else std::snprintf(over, sizeof over, "Verifying checksum...");
        ImGui::ProgressBar(frac, ImVec2(w - 32, phone ? 28 : 20), over);
    } else {
        if (st == 3) { std::lock_guard<std::mutex> l(dlMu_); ImGui::TextColored(vec(0xE8A13A), "Failed: %s", dlMsg_.c_str()); }
        ImGui::PushStyleColor(ImGuiCol_Button, vec(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec(0x9AD3FF));
        ImGui::PushStyleColor(ImGuiCol_Text, vec(kBg));
        if (ImGui::Button(st == 3 ? "Retry download" : "Download", ImVec2(140, bh))) startPackDownload();
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(90, bh))) packCard_ = false;  // secondary, right of Download
        if (!phone) ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(vec(kMuted), "prebuilt, checksum-verified");
    }
    ImGui::Spacing();
    if (!phone) ImGui::SetNextItemOpen(manualOpen_, ImGuiCond_Always);
    if (!phone) manualOpen_ = ImGui::TreeNodeEx("Manual install", ImGuiTreeNodeFlags_NoTreePushOnOpen);  // shell build: desktop only
    if (!phone && manualOpen_) {
        ImGui::PushTextWrapPos(w - 16);
        ImGui::TextColored(vec(kMuted), "Build it yourself instead: streams the %s dbSNP file from UCSC (about 10 minutes, needs bcftools).", kRsidPackStream);
        ImGui::PopTextWrapPos();
        ImGui::PushFont(fonts().mono);
        ImGui::TextColored(vec(kAccent), "scripts/fetch_rsid_pack.sh");
        ImGui::PopFont();
        if (ImGui::SmallButton("Copy command")) ImGui::SetClipboardText("scripts/fetch_rsid_pack.sh");
    }
    ImGui::Spacing();
    bool showsDl = !rsid_.present() && st != 1;
    if (!showsDl) { if (ImGui::Button(st == 1 ? "Hide" : "Close", ImVec2(90, bh))) packCard_ = false; }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) packCard_ = false;
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
}

void App::drawTopBar(ImDrawList* dl, float w) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, kTopBarH), col(kPanel));
    dl->AddLine(ImVec2(0, kTopBarH - 0.5f), ImVec2(w, kTopBarH - 0.5f), col(kBorder));
    // flame mark
    ImVec2 c(kMargin + 9, kTopBarH * 0.5f);
    dl->AddCircleFilled(c, 9, col(kAccent, 0.18f));
    dl->AddCircleFilled(c, 4.5f, col(kAccent));
    ImGui::PushFont(fonts().title);
    float th = ImGui::GetTextLineHeight();
    dl->AddText(ImVec2(kMargin + 26, (kTopBarH - th) * 0.5f), col(kText), "ProjectWisp");
    float tx = kMargin + 26 + ImGui::CalcTextSize("ProjectWisp").x + 28;
    // headline computed from the summary (exon + cCRE outside exons), never hardcoded
    double known = ov_.share(kExon) + ov_.share(kCcre);
    char head[64];
    std::snprintf(head, sizeof head, "%.1f%%", known * 100.0);
    dl->AddText(ImVec2(tx, (kTopBarH - th) * 0.5f), col(kAccent), head);
    float hx = tx + ImGui::CalcTextSize(head).x + 6;
    ImGui::PopFont();
    ImGui::PushFont(fonts().title);
    dl->AddText(ImVec2(hx, (kTopBarH - th) * 0.5f), col(kText, 0.92f), "has a known function");
    ImGui::PopFont();
    const char* ref = "T2T-CHM13 v2.0 (hs1)";
    ImVec2 rs = ImGui::CalcTextSize(ref);
    dl->AddText(ImVec2(w - kMargin - rs.x, (kTopBarH - rs.y) * 0.5f), col(kMuted), ref);
}

float App::drawCoverage(ImDrawList* dl, float y0, float w) {
    struct Seg { const char* name; double share; unsigned color; const char* desc; };
    double exon = ov_.share(kExon), ccre = ov_.share(kCcre);
    Seg segs[4] = {
        {"Known function", exon + ccre, kColKnown, "Exons plus ENCODE regulatory elements (cCREs)."},
        {"Introns", ov_.share(kIntron), kColIntron, "Inside genes, spliced out of the RNA."},
        {"Repeats", ov_.share(kRepeat), kColRepeat, "Identified repeat families, function mostly unknown."},
        {"Nothing known", ov_.share(kNothing), kColFog, "No gene, regulatory or repeat annotation."},
    };
    double notIn[kCatCount];
    ov_.notInByCategory(notIn);
    double notInSeg[4] = {notIn[kExon] + notIn[kCcre], notIn[kIntron], notIn[kRepeat], notIn[kNothing]};
    const float barY = y0 + 14, barH = 8, gap = 2;
    const float x0 = kMargin, usable = w - 2 * kMargin - gap * 3;
    float x = x0;
    ImVec2 mouse = ImGui::GetIO().MousePos;
    int hovered = -1;
    bool hoverHatch = false;
    for (int i = 0; i < 4; ++i) {
        float sw = float(segs[i].share) * usable;
        ImVec2 a(x, barY), b(x + sw, barY + barH);
        if (i == 0) {
            float ew = float(exon) * usable;
            dl->AddRectFilled(a, ImVec2(x + ew, b.y), col(kColKnown));
            dl->AddRectFilled(ImVec2(x + ew, a.y), b, col(kColCcre));
        } else {
            dl->AddRectFilled(a, b, col(segs[i].color));
        }
        // 'Not in GRCh38' share of this segment as a hatched overlay at its right end
        float hw = float(notInSeg[i]) * usable;
        if (hw > 0.5f) hatch(dl, b.x - hw, a.y, b.x, b.y);
        if (mouse.x >= a.x && mouse.x < b.x + gap && mouse.y >= a.y - 6 && mouse.y < b.y + 6) {
            hovered = i;
            hoverHatch = mouse.x >= b.x - hw;
        }
        x = b.x + gap;
    }
    // legend
    float ly = barY + barH + 9;
    float lx = x0;
    ImGui::PushFont(fonts().ui);
    auto legend = [&](unsigned c, const char* label, double share, bool isHatch) {
        if (isHatch) {
            dl->AddRectFilled(ImVec2(lx, ly + 3), ImVec2(lx + 10, ly + 13), col(kBg));
            hatch(dl, lx, ly + 3, lx + 10, ly + 13);
            dl->AddRect(ImVec2(lx, ly + 3), ImVec2(lx + 10, ly + 13), col(kColNotIn));
        } else {
            dl->AddRectFilled(ImVec2(lx, ly + 3), ImVec2(lx + 10, ly + 13), col(c), 2);
        }
        lx += 16;
        std::string t = std::string(label) + "  " + pct(share);
        dl->AddText(ImVec2(lx, ly), col(kMuted), t.c_str());
        lx += ImGui::CalcTextSize(t.c_str()).x + 22;
    };
    legend(kColKnown, "Exons", exon, false);
    legend(kColCcre, "Regulatory (cCRE)", ccre, false);
    legend(kColIntron, "Introns", segs[1].share, false);
    legend(kColRepeat, "Repeats", segs[2].share, false);
    legend(kColFog, "Nothing known", segs[3].share, false);
    legend(0, "Not in GRCh38", ov_.share(kNotIn), true);
    ImGui::PopFont();
    ImGui::PushFont(fonts().monoSmall);
    dl->AddText(ImVec2(lx + 8, ly + 2), col(kMuted, 0.85f), "overview: brightness is relative to the genome-wide spread");
    ImGui::PopFont();

    if (hovered >= 0) {
        ImGui::BeginTooltip();
        ImGui::PushFont(fonts().uiBold);
        ImGui::Text("%s  %s", segs[hovered].name, pct(segs[hovered].share).c_str());
        ImGui::PopFont();
        ImGui::TextColored(vec(kMuted), "%s", segs[hovered].desc);
        if (hovered == 0) {
            ImGui::Text("Exons %s", pct(exon).c_str());
            ImGui::Text("cCREs outside exons %s", pct(ccre).c_str());
        }
        ImGui::Text("%s of %s bp", commas(int64_t(segs[hovered].share * double(ov_.genomeLength()))).c_str(),
                    commas(int64_t(ov_.genomeLength())).c_str());
        if (hoverHatch || hovered == 3) {
            ImGui::Separator();
            ImGui::TextColored(vec(0xB89BE8), "Not in GRCh38: %s of genome (~%s within this segment)",
                               pct(ov_.share(kNotIn)).c_str(), pct(notInSeg[hovered]).c_str());
        }
        ImGui::EndTooltip();
    }
    return ly + 30;
}

void App::drawOverview(ImDrawList* dl, ImVec2 A, ImVec2 B, FogRenderer& fog) {
    dl->AddRectFilled(A, B, col(kPanel), 8);
    dl->AddRect(A, B, col(kBorder), 8);
    // one column of strips: 6 px tall, 3 px gap, 40 px label gutter; one solid block per 100 kb bin
    const int n = int(order_.size());
    if (!n) return;
    const float stripH = 6.f, gap = 3.f, gutter = 40.f;
    const float colH = n * stripH + (n - 1) * gap;
    const float top = A.y + std::max(4.f, ((B.y - A.y) - colH) * 0.5f);
    const float miniW = 40.f, miniGap = 12.f;
    const float sx0 = A.x + 12 + gutter, sxMax = B.x - 14 - miniW - miniGap;
    double maxLen = 0;
    for (int ci : order_) maxLen = std::max(maxLen, double(ov_.chroms()[ci].length));
    std::vector<FogRect> fogRects;
    struct Hatch { float x0, y0, x1, y1; };
    std::vector<Hatch> hatches;
    ImGuiIO& io = ImGui::GetIO();
    const double bin = ov_.binSize();
    {  // header above the mini-bar column
        ImGui::PushFont(fonts().monoTiny);
        const char* h = "share";
        float hx = B.x - 14 - miniW * 0.5f - ImGui::CalcTextSize(h).x * 0.5f;
        dl->AddText(ImVec2(hx, top - 12), col(0x6B7280), h);
        ImGui::PopFont();
    }
    ImGui::PushFont(fonts().monoSmall);
    for (int r = 0; r < n; ++r) {
        int ci = order_[r];
        const auto& ch = ov_.chroms()[ci];
        float y = top + r * (stripH + gap), yb = y + stripH;
        float sw = float((sxMax - sx0) * double(ch.length) / maxLen);
        double bpPerPx = double(ch.length) / sw;
        std::string label = ch.name.substr(0, 3) == "chr" ? ch.name.substr(3) : ch.name;
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        const bool isY = ch.name == "chrY";
        // chrY in hs1 comes from HG002, not CHM13: label in a quieter grey (see tooltip)
        unsigned lc = ci == viewCi_ ? kAccent : isY ? 0x8A8F99 : kMuted;
        dl->AddText(ImVec2(sx0 - 6 - ts.x, y + (stripH - ts.y) * 0.5f), col(lc), label.c_str());
        auto bx = [&](uint32_t b) { return sx0 + float(std::min<double>(double(b) * bin, double(ch.length)) / bpPerPx); };
        // bin colour: intron/repeat grey mix by share, blue over it by known share (x4), then fog by nothing share
        // (the fog layer is drawn by the noise shader so it keeps drifting)
        auto mixc = [](ImVec4 a, ImVec4 b, float t) {
            return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.f);
        };
        int hatchB = -1;
        int fogStart = -1;
        float fogA = 0;
        for (uint32_t b = 0; b <= ch.nbins; ++b) {
            bool last = b == ch.nbins;
            float qa = 0;
            if (!last) {
                float fi = ov_.frac(ci, b, kIntron), fr = ov_.frac(ci, b, kRepeat);
                float known = ov_.frac(ci, b, kExon) + ov_.frac(ci, b, kCcre);
                ImVec4 c = fi + fr > 0 ? mixc(vec(kColIntron), vec(kColRepeat), fr / (fi + fr)) : vec(kColIntron);
                c = mixc(c, vec(kColKnown), stretchAlpha(known, knownP5_, knownP95_));
                float x0 = bx(b), x1 = std::max(bx(b + 1), x0 + 0.5f);
                dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, yb), ImGui::ColorConvertFloat4ToU32(c));
                qa = std::round(stretchAlpha(ov_.frac(ci, b, kNothing), nothingP5_, nothingP95_) * 20.f) / 20.f;  // quantised so runs merge
            }
            if (fogStart >= 0 && (last || qa != fogA)) {
                fogRects.push_back({bx(uint32_t(fogStart)), y, bx(b), yb, fogA, 0.f, 0.f});
                fogStart = -1;
            }
            if (!last && qa > 0 && fogStart < 0) { fogStart = int(b); fogA = qa; }
            bool ni = !last && ov_.frac(ci, b, kNotIn) > 0.5f;
            if (ni && hatchB < 0) hatchB = int(b);
            if (!ni && hatchB >= 0) { hatches.push_back({bx(uint32_t(hatchB)), y, bx(b), yb}); hatchB = -1; }
        }
        // per-chromosome mini bar at the strip's right end: known | introns+repeats | nothing known
        {
            float mx = B.x - 14 - miniW, x = mx;  // one aligned column at the right edge
            double sh[3] = {ch.share(kExon) + ch.share(kCcre), ch.share(kIntron) + ch.share(kRepeat), ch.share(kNothing)};
            unsigned cc[3] = {kColKnown, kColIntron, kColFog};
            double tot = sh[0] + sh[1] + sh[2];
            for (int k = 0; k < 3; ++k) {
                float w = tot > 0 ? float(miniW * sh[k] / tot) : 0.f;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, yb), col(cc[k]));
                x += w;
            }
            // not-in-GRCh38 share as hatching over the right end of the mini bar
            float hw = float(miniW * std::clamp(ch.share(kNotIn), 0.0, 1.0));
            if (ch.share(kNotIn) >= 0.025) {  // < 2.5% would be under 1 px
                dl->AddRectFilled(ImVec2(mx + miniW - hw, y), ImVec2(mx + miniW, yb), col(kBg, 0.35f));
                hatch(dl, mx + miniW - hw, y, mx + miniW, yb, 1.f);
            }
            if (ImGui::IsMouseHoveringRect(ImVec2(mx, y - 1), ImVec2(mx + miniW, yb + 1))) {
                ImGui::BeginTooltip();
                ImGui::PushFont(fonts().uiBold);
                ImGui::Text("%s  %.1f Mb", ch.name.c_str(), double(ch.length) / 1e6);
                ImGui::PopFont();
                ImGui::TextColored(vec(kColKnown), "Known function  %.1f%%", 100 * sh[0]);
                ImGui::TextColored(vec(kMuted), "Introns + repeats  %.1f%%", 100 * sh[1]);
                ImGui::Text("Nothing known  %.1f%%", 100 * sh[2]);
                ImGui::TextColored(vec(0xC9B6F2), "New in T2T (not in GRCh38)  %.1f%%", 100 * ch.share(kNotIn));
                if (isY) ImGui::TextColored(vec(0x8A8F99), "From a different person (HG002); CHM13 has no Y.");
                ImGui::EndTooltip();
            }
        }
        // interaction: click = whole chromosome, drag = zoom to the selected range
        ImGui::SetCursorScreenPos(ImVec2(sx0, y - gap * 0.5f));
        ImGui::PushID(r);
        ImGui::InvisibleButton("strip", ImVec2(std::max(sw, 4.f), stripH + gap));
        if (ImGui::IsItemActivated()) { dragCi_ = ci; dragX0_ = io.MousePos.x; }
        if (ImGui::IsItemHovered()) {
            double pos = std::clamp<double>((io.MousePos.x - sx0) * bpPerPx, 0, double(ch.length) - 1);
            hover_ = {true, ci, int64_t(pos), true};
        }
        if (ImGui::IsItemActive() && dragCi_ == ci) {
            float a = std::min(dragX0_, io.MousePos.x), b = std::max(dragX0_, io.MousePos.x);
            dl->AddRectFilled(ImVec2(a, y - 2), ImVec2(b, yb + 2), col(kAccent, 0.18f));
            dl->AddRect(ImVec2(a, y - 2), ImVec2(b, yb + 2), col(kAccent));
        }
        if (ImGui::IsItemDeactivated() && dragCi_ == ci) {
            float a = std::min(dragX0_, io.MousePos.x), b = std::max(dragX0_, io.MousePos.x);
            if (b - a < 3) setView(ci, 0, double(ch.length));
            else setView(ci, (a - sx0) * bpPerPx, (b - sx0) * bpPerPx);
            dragCi_ = -1;
        }
        ImGui::PopID();
        if (ci == viewCi_) {
            float a = sx0 + float(viewS_ / bpPerPx), b = sx0 + float(viewE_ / bpPerPx);
            if (b - a < 3) { float m = (a + b) * 0.5f; a = m - 1.5f; b = m + 1.5f; }
            dl->AddRect(ImVec2(a - 1, y - 1.5f), ImVec2(b + 1, yb + 1.5f), col(kAccent), 1, 0, 1.f);
        }
    }
    ImGui::PopFont();
    fog.draw(dl, fogRects, 24.f);  // overview: fog alpha = percentile-stretched nothing share, first noise octave at 24 px
    for (auto& h : hatches) hatch(dl, h.x0, h.y0, h.x1, h.y1);
}

void App::drawRegion(ImDrawList* dl, ImVec2 A, ImVec2 B, FogRenderer& fog) {
    dl->AddRectFilled(A, B, col(kPanel), 8);
    dl->AddRect(A, B, col(kBorder), 8);
    ImGuiIO& io = ImGui::GetIO();
    const auto& ch = ov_.chroms()[viewCi_];
    const bool phone = opt_.phone;
    const float ts = phone ? 1.5f : 1.f;  // phone: track heights x1.5, labels in a 56 dp gutter, hints hidden
    const float gutter = phone ? 72 - 12 + 4 : 84;
    const float tx0 = A.x + 12 + gutter, tx1 = B.x - (phone ? 8 : 14), W = tx1 - tx0;
    regionWidthPx_ = W;
    regionTx0_ = tx0;
    const double span = viewE_ - viewS_, bpPerPx = span / W, pxPerBp = W / span;
    auto X = [&](double pos) { return float(tx0 + (pos - viewS_) * pxPerBp); };

    // header: locus + go-to box
    std::string locus = ch.name + ":" + commas(int64_t(viewS_) + 1) + "-" + commas(int64_t(viewE_));
    if (phone) {  // compact header: locus + span only
        ImGui::PushFont(fonts().mono);
        dl->AddText(ImVec2(A.x + 12, A.y + 10), col(kAccent), locus.c_str());
        float lw = ImGui::CalcTextSize(locus.c_str()).x;
        ImGui::PopFont();
        ImGui::PushFont(fonts().ui13);
        dl->AddText(ImVec2(A.x + 12 + lw + 8, A.y + 10), col(kMuted), spanStr(span).c_str());
        ImGui::PopFont();
    } else {
    ImGui::PushFont(fonts().uiBold);
    dl->AddText(ImVec2(A.x + 12, A.y + 10), col(kText), "Region");
    ImGui::PopFont();
    ImGui::PushFont(fonts().mono);
    dl->AddText(ImVec2(A.x + 74, A.y + 11), col(kAccent), locus.c_str());
    float lw = ImGui::CalcTextSize(locus.c_str()).x;
    ImGui::PopFont();
    dl->AddText(ImVec2(A.x + 74 + lw + 12, A.y + 10), col(kMuted), (spanStr(span) + "   wheel zoom | drag pan | click pin").c_str());
    }

    // track heights (genes 48, cCRE 24, repeats 24); each track has a 14px header line for hints
    const float varH = 20 * ts;
    const float hdrH = 14, geneTrackH = 48 * ts, ccreH = 24 * ts, repH = 24 * ts, niH = 10 * ts;
    const float stackH = 26 + (10 + 8) + (hdrH + 20 + 6) + (hdrH + geneTrackH + 6) + (hdrH + ccreH + 6) + (hdrH + repH + 6) + niH + (6 + hdrH + varH) + (6 + hdrH + (svExpanded_ ? std::max(20.f, svRowsLast_ * 7.f + 5.f) : 20.f));
    const float availBot = B.y - 30;
    (void)availBot; (void)stackH;
    float y = A.y + (phone ? 24 : 28) + 16;  // top-aligned, 16px under the Region header
    auto zoomNote = [&](float ly, float lh, const char* t) {  // phone: lane empty because of zoom, centred note
        if (!phone) return;
        ImGui::PushFont(fonts().uiSmall);  // 12 sp on phones
        ImVec2 ns = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(tx0 + (W - ns.x) * 0.5f, ly + (lh - ns.y) * 0.5f), col(0x6B7280), t);
        ImGui::PopFont();
    };
    auto hint = [&](float hy, const char* t) {
        if (phone) return;  // hints hidden on phones
        ImGui::PushFont(fonts().uiSmall);
        dl->AddText(ImVec2(tx0, hy + 1), col(0x6B7280), t);
        ImGui::PopFont();
    };
    // ruler
    {
        double raw = span / (phone ? std::max(2.0, double(W) / 100.0) : 8.0), p10 = std::pow(10.0, std::floor(std::log10(raw)));
        double step = raw / p10 < 2 ? 2 * p10 : raw / p10 < 5 ? 5 * p10 : 10 * p10;
        step = std::max(1.0, step);
        dl->AddLine(ImVec2(tx0, y + 18), ImVec2(tx1, y + 18), col(kBorder));
        ImGui::PushFont(fonts().mono);
        for (double p = std::ceil(viewS_ / step) * step; p <= viewE_; p += step) {
            float x = X(p);
            dl->AddLine(ImVec2(x, y + 13), ImVec2(x, y + 18), col(kMuted));
            std::string t = commas(int64_t(p));
            float tw = ImGui::CalcTextSize(t.c_str()).x;
            if (x - tw * 0.5f > tx0 - 30 && x + tw * 0.5f < tx1 + 10) dl->AddText(ImVec2(x - tw * 0.5f, y - 2), col(kMuted), t.c_str());
        }
        ImGui::PopFont();
        y += 26;
    }
    const float tracksTop = y;
    auto laneLabel = [&](float ly, float lh, const char* t) {
        if (phone) {  // 11 dp labels in the 56 dp gutter; "Not in GRCh38" wraps
            ImGui::PushFont(fonts().uiSmall);
            const float lw = tx0 - A.x - 6;  // 72 dp gutter: single line, cut with an ellipsis if it does not fit
            std::string lt = t;
            if (ImGui::CalcTextSize(t).x > lw) {
                while (!lt.empty() && ImGui::CalcTextSize((lt + "\xE2\x80\xA6").c_str()).x > lw) lt.pop_back();
                while (!lt.empty() && lt.back() == ' ') lt.pop_back();
                lt += "\xE2\x80\xA6";
            }
            ImVec2 ls = ImGui::CalcTextSize(lt.c_str());
            dl->AddText(ImVec2(A.x + 4, ly + (lh - ls.y) * 0.5f), col(kMuted), lt.c_str());
            ImGui::PopFont();
            return;
        }
        ImVec2 ls = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(A.x + 12, ly + (lh - ls.y) * 0.5f), col(kMuted), t);
    };
    std::vector<FogRect> fogRects;
    struct Hatch { float x0, y0, x1, y1; };
    std::vector<Hatch> hatches;
    const int cols = std::max(1, int(W));
    const bool detailed = span <= double(kTrackMaxSpan);
    std::vector<uint8_t> cat(cols, kNothing);  // per-pixel category for the fog
    std::vector<uint8_t> catKnown(cols, 1);     // 0 while that pixel's data is still loading

    // lanes
    const float catY = y, catH = 10 * ts;
    y += catH + 8;
    const float seqHdrY = y;
    const float seqY = y + hdrH, seqH = 20 * ts;
    y = seqY + seqH + 6;
    const float geneHdrY = y;
    const float geneY = y + hdrH;
    const float rowH = geneTrackH / 2;
    const int maxRows = 2;

    Visible vis;
    if (detailed) {
        gather(viewCi_, int64_t(viewS_), int64_t(std::ceil(viewE_)), vis);
        auto paint = [&](int64_t s, int64_t e, uint8_t c) {
            int p0 = std::max(0, int(std::floor((s - viewS_) * pxPerBp)));
            int p1 = std::min(cols, std::max(p0 + 1, int(std::ceil((e - viewS_) * pxPerBp))));
            for (int p = p0; p < p1; ++p) cat[p] = std::min(cat[p], c);
        };
        for (auto* r : vis.repeats) paint(r->s, r->e, kRepeat);
        for (auto* g : vis.genes) paint(g->s, g->e, kIntron);
        for (auto* c : vis.ccres) paint(c->s, c->e, kCcre);
        for (auto* x : vis.exons) paint(x->s, x->e, kExon);
        if (vis.loading[0] || vis.loading[1] || vis.loading[2]) std::fill(catKnown.begin(), catKnown.end(), 0);
    } else {
        for (int p = 0; p < cols; ++p) {
            float f[kCatCount];
            ov_.average(viewCi_, viewS_ + p * bpPerPx, viewS_ + (p + 1) * bpPerPx, f);
            float known = f[kExon] + f[kCcre];
            int best = kNothing;
            float bv = f[kNothing];
            if (f[kRepeat] > bv) { best = kRepeat; bv = f[kRepeat]; }
            if (f[kIntron] > bv) { best = kIntron; bv = f[kIntron]; }
            if (known > bv) best = f[kExon] >= f[kCcre] ? kExon : kCcre;
            cat[p] = uint8_t(best);
        }
    }
    // category strip (runs of equal category)
    laneLabel(catY - 3, catH + 6, "Category");
    for (int p = 0; p < cols;) {
        int q = p;
        while (q < cols && cat[q] == cat[p] && catKnown[q] == catKnown[p]) ++q;
        dl->AddRectFilled(ImVec2(tx0 + p, catY), ImVec2(tx0 + q, catY + catH),
                          catKnown[p] ? col(catColor(cat[p])) : col(kHover));
        p = q;
    }

    // sequence lane
    laneLabel(seqY, seqH, "Sequence");
    bool seqClaim = false;
    if (phone && !genome_.present()) {  // genome pack missing: inline row that opens Downloads
        std::string t = "Sequence not installed (" + mbStr(packBytes("genome")) + "), get it";
        ImGui::PushFont(fonts().ui13);
        ImVec2 ls = ImGui::CalcTextSize(t.c_str());
        ImVec2 p0(tx0, seqY + (seqH - ls.y) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(tx0, seqY + seqH * 0.5f - 22));
        if (ImGui::InvisibleButton("##getgenome", ImVec2(std::min(W, ls.x + 8), 44))) phoneDownloads_ = true;
        seqClaim = ImGui::IsItemHovered() || ImGui::IsItemActive() || ImGui::IsItemDeactivated();
        dl->AddText(p0, col(0x8A8F99), t.c_str());
        float tail = ImGui::CalcTextSize("get it").x;
        dl->AddLine(ImVec2(p0.x + ls.x - tail, p0.y + ls.y), ImVec2(p0.x + ls.x, p0.y + ls.y), col(0x8A8F99));
        ImGui::PopFont();
    } else if (span <= 20000) {
        int64_t s = int64_t(viewS_), e = int64_t(std::ceil(viewE_));
        std::string seq = genome_.fetch(ch.name, s, e);
        ImGui::PushFont(fonts().mono);
        for (size_t i = 0; i < seq.size(); ++i) {
            double pos = double(s + int64_t(i));
            float x0 = X(pos), x1 = X(pos + 1);
            if (x1 < tx0 || x0 > tx1) continue;
            char b = seq[i];
            bool soft = std::islower((unsigned char)b);
            if (pxPerBp >= 8) {
                dl->AddRectFilled(ImVec2(x0 + 0.5f, seqY), ImVec2(x1 - 0.5f, seqY + seqH), col(baseCol(b), soft ? 0.10f : 0.18f), 2);
                char t[2] = {char(std::toupper((unsigned char)b)), 0};
                ImVec2 ts = ImGui::CalcTextSize(t);
                dl->AddText(ImVec2((x0 + x1 - ts.x) * 0.5f, seqY + (seqH - ts.y) * 0.5f), col(baseCol(b), soft ? 0.6f : 1.f), t);
            } else {
                dl->AddRectFilled(ImVec2(x0, seqY + 4), ImVec2(std::max(x1, x0 + 1), seqY + seqH - 4), col(baseCol(b), soft ? 0.5f : 0.9f));
            }
        }
        ImGui::PopFont();
    } else {
        hint(seqHdrY, "zoom below 20 kb to see bases");
        zoomNote(seqY, seqH, "Pinch in to see bases");
    }

    // genes
    const float geneH = geneTrackH;
    if (detailed) {
        std::vector<const Gene*> genes = vis.genes;
        std::sort(genes.begin(), genes.end(), [](auto* a, auto* b) { return a->s < b->s; });
        std::vector<float> rowEnd;
        std::map<std::string, std::vector<std::pair<int, const Gene*>>> byName;
        std::vector<std::pair<int, const Gene*>> placed;
        ImGui::PushFont(fonts().monoSmall);
        for (auto* g : genes) {
            float gx0 = std::max(tx0, X(double(g->s))), gx1 = std::min(tx1, X(double(g->e)));
            float lw2 = ImGui::CalcTextSize(g->name.c_str()).x;
            float end = std::max(gx1, gx0 + lw2) + 10;
            int row = -1;
            for (int r = 0; r < int(rowEnd.size()); ++r)
                if (rowEnd[r] <= gx0) { row = r; break; }
            if (row < 0 && int(rowEnd.size()) < maxRows) { row = int(rowEnd.size()); rowEnd.push_back(0); }
            if (row < 0) row = maxRows - 1;  // overflow: stack on the last row
            rowEnd[row] = std::max(rowEnd[row], end);
            placed.push_back({row, g});
            byName[g->name].push_back({row, g});
        }
        int rows = std::max(1, int(rowEnd.size()));
        (void)rows;
        // phone: skip labels that would overlap another label on the same row, longer genes first (tap still names it)
        std::set<const Gene*> showLabel;
        {
            std::vector<std::pair<int, const Gene*>> byLen = placed;
            std::stable_sort(byLen.begin(), byLen.end(), [](auto& a, auto& b) { return a.second->e - a.second->s > b.second->e - b.second->s; });
            std::vector<std::tuple<int, float, float>> taken;
            for (auto& [row, g] : byLen) {
                float lx = std::clamp(X(double(g->s)), tx0, std::max(tx0, tx1 - 60));
                float lx1 = lx + ImGui::CalcTextSize(g->name.c_str()).x;
                bool clash = false;
                for (auto& [r2, a, b] : taken) clash |= r2 == row && lx < b + 4 && a < lx1 + 4;
                if (!phone || !clash) { showLabel.insert(g); taken.push_back({row, lx, lx1}); }
            }
        }
        for (auto& [row, g] : placed) {
            float ry = geneY + row * rowH;
            float gx0 = X(double(g->s)), gx1 = std::max(X(double(g->e)), gx0 + 1);
            float bandY = ry + 17;
            geneBand(dl, std::max(gx0, tx0), std::min(gx1, tx1), bandY);
            // strand chevrons on the intron band
            if (gx1 - gx0 > 40) {
                float dir = g->strand == '-' ? -1.f : 1.f;
                for (float cx = std::max(gx0, tx0) + 20; cx < std::min(gx1, tx1) - 10; cx += 40)
                    strandChevron(dl, cx, bandY, dir);
            }
            double ht = clock_ - hlT0_;
            if (ht >= 0 && ht < 1.5 && g->name == hlName_ && g->s == hlS_ && ch.name == hlChrom_) {
                float a = float(1.0 - ht / 1.5);
                dl->AddRect(ImVec2(std::max(gx0, tx0) - 3, ry - 1), ImVec2(std::min(gx1, tx1) + 3, ry + rowH - 2), col(kAccent, a), 3, 0, 1.5f);
            }
            float lx = std::clamp(gx0, tx0, std::max(tx0, tx1 - 60));
            if (showLabel.count(g)) dl->AddText(ImVec2(lx, ry + 1), col(kText, 0.9f), g->name.c_str());
        }
        for (auto* x : vis.exons) {
            auto it = byName.find(x->gene);
            if (it == byName.end()) continue;
            for (auto& [row, g] : it->second) {
                if (x->s < g->s || x->e > g->e) continue;
                float ry = geneY + row * rowH;
                float x0 = std::max(tx0, X(double(x->s))), x1 = std::min(tx1, std::max(X(double(x->e)), X(double(x->s)) + 1.f));
                exonBlock(dl, x0, x1, ry + 12, ry + 22);
            }
        }
        ImGui::PopFont();
        if (genes.empty() && !vis.loading[0]) hint(geneHdrY, "no genes here");
    } else {
        for (int p = 0; p < cols; ++p) {
            float f[kCatCount];
            ov_.average(viewCi_, viewS_ + p * bpPerPx, viewS_ + (p + 1) * bpPerPx, f);
            float gb = std::min(1.f, f[kIntron] + f[kExon]);
            if (gb > 0.02f) dl->AddRectFilled(ImVec2(tx0 + p, geneY + 16), ImVec2(tx0 + p + 1, geneY + 24), col(kColIntron, gb));
            if (f[kExon] > 0.002f) dl->AddRectFilled(ImVec2(tx0 + p, geneY + 12), ImVec2(tx0 + p + 1, geneY + 28), col(kColKnown, std::min(1.f, 0.25f + f[kExon] * 8)));
        }
        hint(geneHdrY, "density from 100 kb summary - zoom below 3 Mb for features");
    }
    laneLabel(geneY, rowH, "Genes");
    y = geneY + geneH + 6;

    // cCREs, repeats, not-in-GRCh38
    const float ccreY = y + hdrH, repY = ccreY + ccreH + 6 + hdrH, niY = repY + repH + 6;
    laneLabel(ccreY, ccreH, "Regulatory");
    // CpG methylation heat strip (4 px) at the bottom of the regulatory track: per-pixel mean, #2A2F3A -> #C9A6FF
    if (store_.hasMethyl()) {
        const float my0 = ccreY + ccreH - 4, my1 = ccreY + ccreH;
        if (!detailed) {
            hint(ccreY - hdrH, "cCREs (ENCODE)  |  methylation strip: zoom below 3 Mb");
        } else if (vis.loading[int(Track::Methyl)]) {
            hint(ccreY - hdrH, "cCREs (ENCODE)  |  loading CpG methylation...");
        } else {
            std::vector<float> sum(cols, 0.f);
            std::vector<int> n(cols, 0);
            for (auto* m : vis.methyl) {
                int px = int((double(m->s) - viewS_) * pxPerBp);
                if (px < 0 || px >= cols) continue;
                sum[px] += m->frac;
                ++n[px];
            }
            for (int px = 0; px < cols; ++px) {
                if (!n[px]) continue;
                float w = std::max(1.f, float(pxPerBp * 2));  // CpG = 2 bp
                dl->AddRectFilled(ImVec2(tx0 + px, my0), ImVec2(std::min(tx1, tx0 + px + w), my1), methylCol(sum[px] / n[px]));
            }
            hint(ccreY - hdrH, "cCREs (ENCODE)  |  bottom strip: CpG methylation (CHM13 nanopore), dark = low, violet = high");
        }
    }
    laneLabel(repY, repH, "Repeats");
    laneLabel(niY, niH, phone ? "New in T2T" : "Not in GRCh38");
    if (detailed) {
        ImGui::PushFont(fonts().mono);
        for (auto* c : vis.ccres) {
            float x0 = std::max(tx0, X(double(c->s))), x1 = std::min(tx1, std::max(X(double(c->e)), x0 + 1));
            dl->AddRectFilled(ImVec2(x0, ccreY), ImVec2(x1, ccreY + ccreH - 6), col(kColCcre));
        }
        for (auto* r : vis.repeats) {
            float x0 = std::max(tx0, X(double(r->s))), x1 = std::min(tx1, std::max(X(double(r->e)), x0 + 1));
            dl->AddRectFilled(ImVec2(x0, repY), ImVec2(x1, repY + repH), col(kColRepeat));
            if (x1 - x0 > ImGui::CalcTextSize(r->name.c_str()).x + 6)
                dl->AddText(ImVec2(x0 + 3, repY), col(kBg), r->name.c_str());
        }
        ImGui::PopFont();
        for (auto* u : vis.notin) {
            float x0 = std::max(tx0, X(double(u->s))), x1 = std::min(tx1, X(double(u->e)));
            notInFill(dl, x0, niY, x1, niY + niH);
            hatches.push_back({x0, niY, x1, niY + niH});
        }
    } else {
        int hs = -1;
        for (int p = 0; p <= cols; ++p) {
            float f[kCatCount] = {};
            if (p < cols) ov_.average(viewCi_, viewS_ + p * bpPerPx, viewS_ + (p + 1) * bpPerPx, f);
            if (p < cols) {
                if (f[kCcre] > 0.002f) dl->AddRectFilled(ImVec2(tx0 + p, ccreY), ImVec2(tx0 + p + 1, ccreY + ccreH), col(kColCcre, std::min(1.f, f[kCcre] * 6)));
                if (f[kRepeat] > 0.01f) dl->AddRectFilled(ImVec2(tx0 + p, repY), ImVec2(tx0 + p + 1, repY + repH), col(kColRepeat, std::min(1.f, f[kRepeat] * 1.3f)));
            }
            bool ni = p < cols && f[kNotIn] > 0.5f;
            if (ni && hs < 0) hs = p;
            if (!ni && hs >= 0) { hatches.push_back({tx0 + hs, niY, tx0 + p, niY + niH}); hs = -1; }
        }
    }
    // variants: 1 px ticks; when several fall in one pixel, ticks become per-pixel density bars
    const float varHdrY = niY + niH + 6, varY = varHdrY + hdrH;
    laneLabel(varY, varH, "Variants");
    if (!store_.hasVariants()) {
        hint(varHdrY, "no variant file found in data/ (expects a bgzipped VCF + .tbi)");
    } else if (phone && std::find(store_.variantContigs().begin(), store_.variantContigs().end(), ch.name) == store_.variantContigs().end()) {
        std::string on;  // from the file's contigs, e.g. "chr22"
        const auto& vc = store_.variantContigs();
        for (size_t i = 0; i < vc.size() && i < 3; ++i) on += (i ? ", " : "") + vc[i];
        if (vc.size() > 3) on += " and " + std::to_string(vc.size() - 3) + " more";
        zoomNote(varY, varH, ("Variants available on " + on + " only for now").c_str());
    } else if (!detailed) {
        hint(varHdrY, "zoom below 3 Mb to load variants");
        zoomNote(varY, varH, "Pinch in to see variants");
    } else if (vis.loading[int(Track::Variants)]) {
        hint(varHdrY, "loading variants...");
    } else if (vis.variants.empty()) {
        hint(varHdrY, "no variants here (variant file covers chr22 only)");
    } else {
        std::vector<int> cnt(cols, 0);
        std::vector<float> maxAf(cols, 0.f);
        for (auto* v : vis.variants) {
            int px = int((double(v->s) - viewS_) * pxPerBp);
            if (px < 0 || px >= cols) continue;
            ++cnt[px];
            maxAf[px] = std::max(maxAf[px], v->af);
        }
        int mx = *std::max_element(cnt.begin(), cnt.end());
        bool dense = mx > 1;
        char h[160];
        std::snprintf(h, sizeof h, "%s variants in view (1KGP, %s)  %s", commas(int64_t(vis.variants.size())).c_str(),
                      dense ? "density per pixel" : "one tick per variant", "brighter = more common");
        hint(varHdrY, h);
        if (!dense && pxPerBp >= 1) {
            for (auto* v : vis.variants) {
                float x0 = X(double(v->s)), x1 = std::max(X(double(v->e)), x0 + 1.f);
                variantTick(dl, x0, x1, varY, varY + varH, v->af);
            }
        } else {
            for (int p = 0; p < cols; ++p) {
                if (!cnt[p]) continue;
                float hgt = varH * (0.25f + 0.75f * float(cnt[p]) / float(mx));
                dl->AddRectFilled(ImVec2(tx0 + p, varY + varH - hgt), ImVec2(tx0 + p + 1, varY + varH), col(kAccent, variantAlpha(maxAf[p])));
            }
        }
    }
    // structural variants (HPRC pangenome): ins = 2 px tick + caret, del/inv/complex = bars.
    // Collapsed: largest first so smaller events sit on top (covered ones get a 1 px outline).
    // Expanded: greedy sub-rows, 6 px + 1 px gap, max 8 rows, '+N more' for overflow.
    const float svH = svExpanded_ ? std::max(20.f * ts, svRowsLast_ * 7.f - 1.f + 6.f) : 20.f * ts;
    const float svHdrY = varY + varH + 6, svY = svHdrY + hdrH;
    bool uiClaim = seqClaim;  // a UI control under the mouse consumes it: no map hover, pan or pin
    auto svT0 = std::chrono::steady_clock::now();
    laneLabel(svY, std::min(svH, 20.f * ts), "SVs");
    {  // expand / collapse chevron at the right of the header line (44 dp touch target on phones)
        ImVec2 c(tx1 - 6, svHdrY + 7);
        if (phone) {
            ImGui::SetCursorScreenPos(ImVec2(c.x - 30, c.y - 22));
            if (ImGui::InvisibleButton("##svexpand", ImVec2(44, 44))) svExpanded_ = !svExpanded_;
        } else {
        ImGui::SetCursorScreenPos(ImVec2(tx1 - 16, svHdrY - 1));
        if (ImGui::InvisibleButton("##svexpand", ImVec2(16, 15))) svExpanded_ = !svExpanded_;
        }
        uiClaim = ImGui::IsItemHovered() || ImGui::IsItemActive() || ImGui::IsItemDeactivated();
        unsigned cc = ImGui::IsItemHovered() ? kAccent : kMuted;
        if (svExpanded_) dl->AddTriangleFilled(ImVec2(c.x - 4, c.y + 2), ImVec2(c.x + 4, c.y + 2), ImVec2(c.x, c.y - 3), col(cc));
        else dl->AddTriangleFilled(ImVec2(c.x - 4, c.y - 2), ImVec2(c.x + 4, c.y - 2), ImVec2(c.x, c.y + 3), col(cc));
    }
    if (!store_.hasSv()) {
        hint(svHdrY, "no SV file (run scripts/fetch_sv.sh)");
    } else if (!detailed) {
        hint(svHdrY, "zoom below 3 Mb to load structural variants");
        zoomNote(svY, svH, "Pinch in to see structural variants");
    } else if (vis.loading[int(Track::Sv)]) {
        hint(svHdrY, "loading structural variants...");
    } else {
        int nIns = 0, nDel = 0, nCx = 0;
        for (auto* g : vis.svGroups) { nIns += g->type == SvType::Ins; nDel += g->type == SvType::Del; nCx += g->type != SvType::Ins && g->type != SvType::Del; }
        auto extentOf = [&](int64_t s0, int64_t e0, SvType t, float& x0, float& x1) {
            if (t == SvType::Ins) { float x = X(double(s0) + 1.0); x0 = x - 3; x1 = x + 3; }
            else { x0 = X(double(s0)); x1 = std::max(X(double(e0)), x0 + 2.f); }
        };
        auto drawBar = [&](SvType t, bool inv, float af, float x0, float x1, float y0, float y1, bool outline, float mul = 1.f) {
            svBar(dl, t, inv, af, x0, x1, y0, y1, outline, mul, tx0, tx1);
        };
        auto drawIns = [&](int64_t s0, float af, float y0, float y1, bool caret) {
            float x = X(double(s0) + 1.0);
            if (x < tx0 || x > tx1) return;
            svIns(dl, x, af, y0, y1, caret);
        };
        auto groupAf = [](const SvGroup* g) { return g->an > 0 ? float(g->ac) / float(g->an) : -1.f; };
        if (!svExpanded_) {
            // one bar per allele group; order: complex (bottom), deletions, insertions; larger first within a type
            auto rank = [](SvType t) { return t == SvType::Ins ? 2 : t == SvType::Del ? 1 : 0; };
            std::vector<const SvGroup*> gs(vis.svGroups.begin(), vis.svGroups.end());
            std::stable_sort(gs.begin(), gs.end(), [&](auto* a2, auto* b2) {
                if (rank(a2->type) != rank(b2->type)) return rank(a2->type) < rank(b2->type);
                return (a2->e - a2->s) > (b2->e - b2->s);
            });
            std::vector<std::pair<int64_t, int64_t>> cx;  // complex spans (for deletion transparency)
            std::vector<std::pair<float, float>> invEdges;  // violet edges, drawn last
            for (auto* g : gs) if (g->type == SvType::Complex) {
                cx.push_back({g->s, g->e});
                if (g->inv) { float a0, a1; extentOf(g->s, g->e, g->type, a0, a1); invEdges.push_back({a0, a1}); }
            }
            std::map<float, float> drawn;  // union of drawn px extents for the outline test (O(log n) per bar)
            for (auto* g : gs) {
                float x0, x1;
                extentOf(g->s, g->e, g->type, x0, x1);
                auto it = drawn.upper_bound(x0);
                bool covered = (it != drawn.end() && it->first < x1);
                if (it != drawn.begin() && std::prev(it)->second > x0) covered = true;
                if (g->type == SvType::Ins) drawIns(g->s, groupAf(g), svY + 6, svY + svH - 2, true);
                else {
                    float mul = 1.f;
                    if (g->type == SvType::Del)
                        for (auto& c : cx) if (c.first < g->e && g->s < c.second) { mul = 0.85f; break; }
                    drawBar(g->type, false, groupAf(g), x0, x1, svY + 5, svY + svH - 3, covered, mul);
                }
                float m0 = x0, m1 = x1;
                if (it != drawn.begin() && std::prev(it)->second >= x0) { --it; m0 = std::min(m0, it->first); }
                while (it != drawn.end() && it->first <= x1) { m1 = std::max(m1, it->second); it = drawn.erase(it); }
                drawn[m0] = m1;
            }
            for (auto& e : invEdges) {  // final pass: "contains an inversion" edge on top of everything
                float a0 = std::max(tx0, e.first), a1 = std::min(tx1, e.second);
                if (a1 > a0) dl->AddRectFilled(ImVec2(a0, svY + 5), ImVec2(a1, svY + 6), col(kSvInvEdge));
            }
            svRowsLast_ = 1;
        } else {
            // every allele on its own row slot (greedy packing by start)
            std::vector<const Sv*> all(vis.svs.begin(), vis.svs.end());
            std::stable_sort(all.begin(), all.end(), [](auto* a2, auto* b2) { return a2->s < b2->s; });
            const int maxRows = 8;
            std::vector<float> rowEnd;
            int overflow = 0;
            float top = svY + 6;
            for (auto* v : all) {
                float x0, x1;
                SvType t = v->type == SvType::Inv ? SvType::Complex : v->type;
                extentOf(v->s, v->e, t, x0, x1);
                if (x1 < tx0 || x0 > tx1) continue;
                int row = -1;
                for (int r = 0; r < int(rowEnd.size()); ++r) if (rowEnd[r] <= x0) { row = r; break; }
                if (row < 0 && int(rowEnd.size()) < maxRows) { row = int(rowEnd.size()); rowEnd.push_back(-1e9f); }
                if (row < 0) { ++overflow; continue; }
                rowEnd[row] = x1 + 1;
                float ry = top + row * 7.f;
                if (t == SvType::Ins) drawIns(v->s, v->af, ry, ry + 6, false);
                else drawBar(t, v->inv, v->af, x0, x1, ry, ry + 6, false);
            }
            svRowsLast_ = std::max(1, int(rowEnd.size()));
            if (overflow) {
                char more[32];
                std::snprintf(more, sizeof more, "+%d more", overflow);
                ImGui::PushFont(fonts().monoSmall);
                ImVec2 ms = ImGui::CalcTextSize(more);
                float my = top + (svRowsLast_ - 1) * 7.f + 3 - ms.y * 0.5f;
                dl->AddRectFilled(ImVec2(tx1 - ms.x - 6, my), ImVec2(tx1, my + ms.y), col(kPanel, 0.9f));
                dl->AddText(ImVec2(tx1 - ms.x - 3, my), col(kMuted), more);
                ImGui::PopFont();
            }
        }
        char h[220];
        if (vis.svGroups.empty()) std::snprintf(h, sizeof h, "no structural variants >= 50 bp here (HPRC pangenome)");
        else std::snprintf(h, sizeof h, "HPRC pangenome SVs >= 50 bp: %d sites (%d alleles): %d ins, %d del, %d complex  |  bright = AF >= 1%%  |  %s",
                           int(vis.svGroups.size()), int(vis.svs.size()), nIns, nDel, nCx, svExpanded_ ? "one row per allele" : "one bar per site");
        hint(svHdrY, h);
    }
    svMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - svT0).count();
    const float tracksBottom = svY + svH;

    // fog over 'nothing known' columns: Category row only (spanning every lane made full-height stripes when zoomed out)
    for (int p = 0; p < cols;) {
        if (cat[p] != kNothing || !catKnown[p]) { ++p; continue; }
        int q = p;
        while (q < cols && cat[q] == kNothing && catKnown[q]) ++q;
        fogRects.push_back({tx0 + p - 2, catY - 2, tx0 + q + 2, catY + catH + 2, 0.55f, 24.f, 10.f});
        p = q;
    }
    // loading: 40% fog over the lane that is still waiting on tabix
    auto loadingFog = [&](bool on, float ly, float lh) {
        if (on) fogRects.push_back({tx0, ly - 2, tx1, ly + lh + 2, 0.40f, 24.f, 4.f});
    };
    if (detailed) {
        loadingFog(vis.loading[0], geneY, geneH);
        loadingFog(vis.loading[1], ccreY, ccreH);
        loadingFog(vis.loading[2], repY, repH);
        loadingFog(vis.loading[3], niY, niH);
        loadingFog(vis.loading[int(Track::Variants)], varY, varH);
        loadingFog(vis.loading[int(Track::Sv)], svY, svH);
        loadingFog(vis.loading[0] || vis.loading[1] || vis.loading[2], catY, catH);
    }
    dl->PushClipRect(ImVec2(tx0, A.y), ImVec2(tx1, B.y), true);
    fog.draw(dl, fogRects);
    dl->PopClipRect();
    for (auto& h : hatches) hatch(dl, h.x0, h.y0, h.x1, h.y1);

    // footer status
    char status[160];
    std::snprintf(status, sizeof status, "region cache %.1f / %zu MB   loading %zu tile(s)", store_.bytes() / 1048576.0,
                  TrackStore::kBudget >> 20, store_.pending());
    if (!phone) dl->AddText(ImVec2(A.x + 12, B.y - 24), col(kMuted, 0.8f), status);

    if (phone) {
        // touch: one-finger pan, tap to pin, long-press (450 ms, < 10 dp of movement) = hover preview; pinch in framePhone
        ImGui::SetCursorScreenPos(ImVec2(A.x, tracksTop - 26));
        ImGui::InvisibleButton("##region", ImVec2(B.x - A.x, std::max(10.f, B.y - tracksTop + 26)));
        double pos = std::clamp(viewS_ + (io.MousePos.x - tx0) * bpPerPx, 0.0, double(ch.length) - 1);
        Probe here{true, viewCi_, int64_t(pos), false};
        if (!uiClaim && ImGui::IsItemActivated()) { pressT0_ = clock_; longPress_ = false; pinchedThisPress_ = pinching_; }
        if (pinching_) pinchedThisPress_ = true;
        const float moved = io.MouseDragMaxDistanceSqr[0];
        if (!uiClaim && ImGui::IsItemActive() && !pinchedThisPress_) {
            if (!longPress_ && moved < 100.f && clock_ - pressT0_ >= 0.45) longPress_ = true;
            if (longPress_) {
                hover_ = here;
                hoverCursor(dl, io.MousePos.x, tracksTop - 6, tracksBottom + 4);
            } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 10.f)) {
                double d = -io.MouseDelta.x * bpPerPx;
                setView(viewCi_, viewS_ + d, viewE_ + d);
            }
        }
        if (!uiClaim && ImGui::IsItemDeactivated()) {
            if (!longPress_ && !pinchedThisPress_ && moved < 100.f) {
                bool same = pinned_.valid && !pinned_.overview && pinned_.ci == here.ci &&
                            std::abs(double(pinned_.pos - here.pos)) <= std::max(1.0, 8.0 * bpPerPx);
                if (same) pinned_ = {};
                else pinned_ = here;
            }
            longPress_ = false;
        }
        if (pinned_.valid && !pinned_.overview && pinned_.ci == viewCi_ && pinned_.pos >= viewS_ && pinned_.pos <= viewE_)
            pinMarker(dl, X(double(pinned_.pos) + 0.5), tracksTop, tracksBottom);
        return;
    }
    // interaction over the lanes
    ImGui::SetCursorScreenPos(ImVec2(tx0, tracksTop - 26));
    ImGui::InvisibleButton("##region", ImVec2(W, std::max(10.f, tracksBottom - tracksTop + 26)));
    if (!uiClaim && ImGui::IsItemHovered() && !ImGui::IsMouseHoveringRect(dropA_, dropB_, false)) {
        double pos = viewS_ + (io.MousePos.x - tx0) * bpPerPx;
        hover_ = {true, viewCi_, int64_t(std::clamp(pos, 0.0, double(ch.length) - 1)), false};
        if (io.MouseWheel != 0) {
            double f = std::pow(1.25, -io.MouseWheel);
            double ns = pos - (pos - viewS_) * f, ne = pos + (viewE_ - pos) * f;
            setView(viewCi_, ns, ne);
        }
        float x = io.MousePos.x;
        hoverCursor(dl, x, tracksTop - 6, tracksBottom + 4);
    }
    if (!uiClaim && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.f)) {
        double d = -io.MouseDelta.x * bpPerPx;
        setView(viewCi_, viewS_ + d, viewE_ + d);
    }
    if (!uiClaim && ImGui::IsItemDeactivated() && io.MouseDragMaxDistanceSqr[0] < 9.f && hover_.valid) {
        // second click on the pinned spot unpins; elsewhere re-pins
        bool same = pinned_.valid && !pinned_.overview && pinned_.ci == hover_.ci &&
                    std::abs(double(pinned_.pos - hover_.pos)) <= std::max(1.0, 3.0 * bpPerPx);
        if (same) pinned_ = {};
        else pinned_ = hover_;
    }
    if (pinned_.valid && !pinned_.overview && pinned_.ci == viewCi_ && pinned_.pos >= viewS_ && pinned_.pos <= viewE_) {
        float x = X(double(pinned_.pos) + 0.5);
        pinMarker(dl, x, tracksTop, tracksBottom);
    }
}

void App::drawCard(ImDrawList* dl, ImVec2 A, ImVec2 B) {
    // Hover = live preview at 80% opacity; pinned = 100% with accent border + pin icon.
    const bool live = hover_.valid;
    const Probe p = live ? hover_ : pinned_;
    const bool pinnedView = !live && pinned_.valid;
    if (!opt_.phone) {  // phone: the bottom sheet draws the panel
        dl->AddRectFilled(A, B, col(kPanel), 8);
        dl->AddRect(A, B, pinnedView ? col(kAccent) : col(kBorder), 8, 0, 1.f);
    }
    if (pinnedView) {  // pin icon
        ImVec2 c(B.x - 22, A.y + 18);
        dl->AddLine(ImVec2(c.x, c.y + 3), ImVec2(c.x, c.y + 10), col(kAccent), 1.5f);
        dl->AddCircleFilled(c, 4.f, col(kAccent));
    }
    const float pad = 16;
    ImGui::SetCursorScreenPos(ImVec2(A.x + pad, A.y + pad));
    ImGui::BeginChild("##card", ImVec2(B.x - A.x - 2 * pad, B.y - A.y - 2 * pad), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    if (opt_.phone && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.f))
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);  // touch scroll
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, live ? 0.8f : 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
    ImGui::PushTextWrapPos(0.f);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    auto U = [](unsigned hex, float a = 1.f) { return ImGui::GetColorU32(vec(hex, a)); };  // honours style alpha
    const float W = ImGui::GetContentRegionAvail().x;
    auto separator = [&]() {
        ImGui::Dummy(ImVec2(0, 6));
        ImVec2 c = ImGui::GetCursorScreenPos();
        cdl->AddLine(ImVec2(c.x, c.y), ImVec2(c.x + W, c.y), U(kBorder), 1.f);
        ImGui::Dummy(ImVec2(0, 7));
    };
    auto heading = [&](const char* t) {
        ImGui::Dummy(ImVec2(0, 8 - 4));  // 8px above (item spacing supplies 4)
        std::string up(t); for (auto& c : up) c = char(std::toupper((unsigned char)c));
        ImGui::PushFont(fonts().uiSmall);
        ImGui::TextColored(vec(0x8A8F99), "%s", up.c_str());
        ImGui::PopFont();
    };
    auto swatch = [&](unsigned c, bool hatched = false) {
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImVec2 a(cp.x, cp.y + 3), b(cp.x + 12, cp.y + 15);
        if (hatched) {
            cdl->AddRectFilled(a, b, U(kNotIn, 0.3f), 2);
            hatch(cdl, a.x, a.y, b.x, b.y);
            cdl->AddRect(a, b, U(kNotIn), 2);
        } else {
            cdl->AddRectFilled(a, b, U(c), 2);
        }
        ImGui::Dummy(ImVec2(12, 18));
        ImGui::SameLine(0, 8);
    };

    if (!p.valid) {
        heading("LOOKUP");
        ImGui::TextColored(vec(kMuted), "Hover the map for a live preview; click to pin. Esc or a second click unpins.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleVar(2);
        ImGui::EndChild();
        return;
    }
    const auto& ch = ov_.chroms()[p.ci];
    std::string posTxt = ch.name + ":" + commas(p.pos + 1);

    // ---------- gather what is known at this base ----------
    int cat = kNothing;
    bool loading = false, notIn = false;
    Visible v;
    if (p.overview) {
        uint32_t bin = uint32_t(p.pos / ov_.binSize());
        for (int k = 0; k < kNotIn; ++k)
            if (ov_.frac(p.ci, bin, Cat(k)) > ov_.frac(p.ci, bin, Cat(cat))) cat = k;
        notIn = ov_.frac(p.ci, bin, kNotIn) > 0.5f;
    } else {
        gather(p.ci, p.pos, p.pos + 1, v);
        for (int k = 0; k < kTrackCount; ++k) loading |= v.loading[k];
        bool inExon = false;
        for (auto* x : v.exons) inExon |= (x->s <= p.pos && p.pos < x->e);
        cat = inExon ? kExon : !v.ccres.empty() ? kCcre : !v.genes.empty() ? kIntron : !v.repeats.empty() ? kRepeat : kNothing;
        notIn = !v.notin.empty();
    }

    // ---------- header: position + category pill ----------
    ImGui::PushFont(fonts().mono);
    ImVec2 hp = ImGui::GetCursorScreenPos();
    ImGui::TextColored(vec(kText), "%s", posTxt.c_str());
    ImGui::PopFont();
    {
        std::string pillS = loading ? "loading" : p.overview ? std::string("Mostly ") + pillName(cat) : std::string(pillName(cat));
        ImVec2 ts = ImGui::CalcTextSize(pillS.c_str());
        float pw = ts.x + 20, px = hp.x + W - pw - (pinnedView ? 18 : 0), py = hp.y - 2;
        unsigned pc = loading ? kHover : catColor(cat);
        bool darkText = !loading && (cat == kExon || cat == kCcre || cat == kNothing);
        cdl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + 20), U(pc), 10);
        cdl->AddText(ImVec2(px + 10, py + (20 - ts.y) * 0.5f), darkText ? U(kBg) : U(kText), pillS.c_str());
    }
    if (notIn) {
        ImGui::Dummy(ImVec2(0, 2));
        swatch(0, true);
        ImGui::TextColored(vec(0xC9B6F2), "New in T2T");
        ImGui::TextColored(vec(kMuted), "%s", kNotInMsg);
    }

    if (p.overview) {
        separator();
        heading("THIS 100 KB BIN");
        uint32_t bin = uint32_t(p.pos / ov_.binSize());
        const char* names[kCatCount] = {"Exons", "cCREs", "Introns", "Repeats", "Nothing known", opt_.phone ? "New in T2T" : "Not in GRCh38"};
        for (int k = 0; k < kCatCount; ++k) {
            float f = ov_.frac(p.ci, bin, Cat(k));
            ImVec2 cp = ImGui::GetCursorScreenPos();
            ImGui::TextUnformatted(names[k]);
            ImGui::SameLine(W - 52);
            ImGui::TextColored(vec(kMuted), "%5.1f%%", f * 100);
            float bx = cp.x + 118, bw = W - 118 - 60;
            cdl->AddRectFilled(ImVec2(bx, cp.y + 6), ImVec2(bx + bw, cp.y + 12), U(kHover), 3);
            if (k == kNotIn) hatch(cdl, bx, cp.y + 6, bx + bw * f, cp.y + 12);
            else cdl->AddRectFilled(ImVec2(bx, cp.y + 6), ImVec2(bx + bw * f, cp.y + 12), U(catColor(k)), 3);
        }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextColored(vec(kMuted), "Click to open the chromosome, drag to zoom.");
    } else if (!loading) {
        // ---------- what it is ----------
        separator();
        heading("WHAT IT IS");
        {
            // structural variants touching this base (insertions: within 1 bp of the anchor)
            Visible sv;
            gather(p.ci, std::max<int64_t>(0, p.pos - 1), p.pos + 2, sv);
            auto kb = [](int64_t bp) {
                char b2[32];
                if (std::llabs(bp) >= 1000) std::snprintf(b2, sizeof b2, "%.3g kb", double(bp) / 1000.0);
                else std::snprintf(b2, sizeof b2, "%lld bp", (long long)bp);
                return std::string(b2);
            };
            auto describe = [&](const Sv* x) {  // plain description of one allele
                char d[200];
                if (x->type == SvType::Ins) std::snprintf(d, sizeof d, "Insertion of %s bp", commas(x->len).c_str());
                else if (x->type == SvType::Del) std::snprintf(d, sizeof d, "Deletion of %s bp", commas(x->len).c_str());
                else std::snprintf(d, sizeof d, "Complex rearrangement over %s (net %s%s)%s", kb(x->refLen).c_str(),
                                   x->net > 0 ? "+" : "", kb(x->net).c_str(), x->inv ? ", contains an inversion" : "");
                return std::string(d);
            };
            auto groupColor = [](const SvGroup* g) -> unsigned { return g->type == SvType::Ins ? 0x5B8DEF : g->type == SvType::Del ? 0xE8A13A : 0x8A8F99; };
            for (auto* g : sv.svGroups) {
                bool hit = g->type == SvType::Ins ? std::llabs(g->s - p.pos) <= 1 : (g->s <= p.pos && p.pos < g->e);
                if (!hit) continue;
                swatch(groupColor(g));
                if (g->count == 1) {
                    const Sv* x = g->alleles;
                    if (x->an > 0)
                        ImGui::TextColored(vec(kText, 0.95f), "%s, found on %s%.0f%% of chromosome copies (%d of %d)", describe(x).c_str(),
                                           x->merged > 1 ? "at least " : "", 100.0 * x->ac / x->an, x->ac, x->an);
                    else
                        ImGui::TextColored(vec(kText, 0.95f), "%s", describe(x).c_str());
                    continue;
                }
                // group: one entry with a chevron that expands to the allele list
                ImGui::PushID(g);
                ImVec2 cp = ImGui::GetCursorScreenPos();
                char t[160];
                std::snprintf(t, sizeof t, "%d alleles here, together on %s%d of %d copies", g->count, g->atLeast ? "at least " : "", g->ac, g->an);
                float wrapW = ImGui::GetContentRegionAvail().x - 18;
                ImVec2 ts = ImGui::CalcTextSize(t, nullptr, false, wrapW);
                const float rowH = opt_.phone ? std::max(ts.y, 44.f) : ts.y;  // 44 dp touch target on phones
                if (ImGui::InvisibleButton("##grp", ImVec2(ImGui::GetContentRegionAvail().x, rowH))) cardSvExpanded_ = !cardSvExpanded_;
                bool hov = ImGui::IsItemHovered();
                cp.y += (rowH - ts.y) * 0.5f;
                cdl->AddText(nullptr, 0.f, cp, U(kText, 0.95f), t, nullptr, wrapW);
                ImVec2 c(cp.x + wrapW + 10, cp.y + 8);
                unsigned cc = hov ? kAccent : kMuted;
                if (cardSvExpanded_) cdl->AddTriangleFilled(ImVec2(c.x - 4, c.y - 2), ImVec2(c.x + 4, c.y - 2), ImVec2(c.x, c.y + 3), U(cc));
                else cdl->AddTriangleFilled(ImVec2(c.x - 2, c.y - 4), ImVec2(c.x - 2, c.y + 4), ImVec2(c.x + 3, c.y), U(cc));
                ImGui::PopID();
                if (cardSvExpanded_) {
                    ImGui::Indent(20);
                    // identical-looking alleles (same short description and count) collapse into one 'N×' row
                    auto shortDesc = [&](const Sv* x) {
                        if (x->type != SvType::Complex) return describe(x);
                        return "Complex rearrangement over " + kb(x->refLen) + (x->inv ? ", contains an inversion" : "");
                    };
                    std::vector<std::pair<std::string, int>> rows;  // key -> first allele index
                    std::vector<int> mult;
                    for (int i = 0; i < g->count; ++i) {
                        const Sv* x = &g->alleles[i];
                        std::string key = shortDesc(x) + "|" + std::to_string(x->ac) + "|" + std::to_string(x->an) + "|" + std::to_string(x->merged > 1);
                        size_t k = 0;
                        while (k < rows.size() && rows[k].first != key) ++k;
                        if (k == rows.size()) { rows.push_back({key, i}); mult.push_back(0); }
                        ++mult[k];
                    }
                    for (size_t k = 0; k < rows.size(); ++k) {
                        const Sv* x = &g->alleles[rows[k].second];
                        if (mult[k] > 1) {
                            ImGui::TextColored(vec(kText, 0.85f), "%d\xC3\x97 %s (%s%d of %d copies each)", mult[k], shortDesc(x).c_str(),
                                               x->merged > 1 ? "at least " : "", x->ac, x->an);
                        } else {
                            ImGui::TextColored(vec(kText, 0.85f), "%s", describe(x).c_str());
                            ImGui::PushFont(fonts().monoSmall);
                            ImGui::TextColored(vec(kMuted), "  %s%d of %d copies", x->merged > 1 ? "at least " : "", x->ac, x->an);
                            ImGui::PopFont();
                        }
                    }
                    ImGui::Unindent(20);
                }
            }
        }
        if (!v.genes.empty()) {
            for (auto* g : v.genes) {
                // exon/intron number from merged exon blocks of this gene (union over transcripts)
                Visible gv;
                gather(p.ci, g->s, g->e, gv);
                std::vector<std::pair<int64_t, int64_t>> ex;
                for (auto* x : gv.exons)
                    if (x->gene == g->name && x->s >= g->s && x->e <= g->e) ex.push_back({x->s, x->e});
                std::sort(ex.begin(), ex.end());
                std::vector<std::pair<int64_t, int64_t>> blocks;
                for (auto& e : ex) {
                    if (!blocks.empty() && e.first <= blocks.back().second) blocks.back().second = std::max(blocks.back().second, e.second);
                    else blocks.push_back(e);
                }
                if (g->strand == '-') std::reverse(blocks.begin(), blocks.end());
                std::string where;
                int nb = int(blocks.size());
                for (int i = 0; i < nb; ++i) {
                    if (blocks[i].first <= p.pos && p.pos < blocks[i].second) { where = "exon " + std::to_string(i + 1) + " of " + std::to_string(nb); break; }
                    if (i + 1 < nb) {
                        int64_t lo = std::min(blocks[i].second, blocks[i + 1].second), hi = std::max(blocks[i].first, blocks[i + 1].first);
                        if (g->strand == '-') { lo = blocks[i + 1].second; hi = blocks[i].first; }
                        else { lo = blocks[i].second; hi = blocks[i + 1].first; }
                        if (lo <= p.pos && p.pos < hi) { where = "intron " + std::to_string(i + 1) + " of " + std::to_string(nb - 1); break; }
                    }
                }
                if (where.empty()) where = nb ? "outside annotated exons" : "no exons annotated";
                bool loadingGene = gv.loading[0];
                ImGui::PushFont(fonts().uiBold);
                ImGui::TextUnformatted(g->name.c_str());
                ImGui::PopFont();
                ImGui::TextColored(vec(kText, 0.9f), "%s", loadingGene ? "..." : where.c_str());
                ImGui::TextColored(vec(kMuted), "%s strand  |  %s  |  %s", g->strand == '-' ? "minus (-)" : "plus (+)",
                                   g->biotype.empty() ? "gene" : g->biotype.c_str(), spanStr(double(g->e - g->s)).c_str());
            }
            ImGui::PushFont(fonts().monoSmall);
            ImGui::TextColored(vec(kMuted, 0.8f), "exon numbers count merged exons across all transcripts");
            ImGui::PopFont();
        } else {
            Visible nv;
            int64_t r = TrackStore::kTile;
            gather(p.ci, std::max<int64_t>(0, p.pos - r), p.pos + r, nv);
            const Gene* best = nullptr;
            int64_t bd = 0;
            for (auto* g : nv.genes) {
                int64_t d = p.pos < g->s ? g->s - p.pos : p.pos >= g->e ? p.pos - g->e + 1 : 0;
                if (!best || d < bd) { best = g; bd = d; }
            }
            if (nv.loading[0]) ImGui::TextColored(vec(kMuted), "looking for the nearest gene...");
            else if (best) ImGui::TextColored(vec(kText, 0.9f), "No gene here. nearest: %s, %s", best->name.c_str(), spanStr(double(bd)).c_str());
            else ImGui::TextColored(vec(kMuted), "No gene within 1 Mb");
        }

        // ---------- what it might do ----------
        separator();
        heading("WHAT IT MIGHT DO");
        // CpG methylation: the CpG at this base, else mean of CpGs within 100 bp
        if (store_.hasMethyl()) {
            Visible mv;
            gather(p.ci, std::max<int64_t>(0, p.pos - 100), p.pos + 101, mv);
            float exact = -1, sum = 0;
            int nn = 0;
            for (auto* m : mv.methyl) {
                if (int64_t(m->s) <= p.pos && p.pos < int64_t(m->e)) exact = m->frac;
                sum += m->frac;
                ++nn;
            }
            if (exact >= 0 || nn) {
                swatch(0xC9A6FF);
                if (exact >= 0) ImGui::TextColored(vec(kText, 0.95f), "CpG methylation: %.0f%% (CHM13, nanopore)", exact * 100);
                else ImGui::TextColored(vec(kText, 0.95f), "CpG methylation nearby: %.0f%% (%d CpGs within 100 bp; CHM13, nanopore)", sum / nn * 100, nn);
                ImGui::TextColored(vec(kMuted), "High usually means packed, silent DNA; low often marks active promoters and enhancers.");
            }
        }
        if (v.ccres.empty()) {
            ImGui::TextColored(vec(kMuted), "No known regulatory element");
        } else {
            for (auto* c2 : v.ccres) {
                ImVec2 cp = ImGui::GetCursorScreenPos();
                ImGui::Indent(11);
                ImGui::PushFont(fonts().uiBold);
                ImGui::TextUnformatted(c2->cls.empty() ? "cCRE" : c2->cls.c_str());
                ImGui::PopFont();
                const char* blurb = c2->cls.find("PLS") != std::string::npos ? "promoter-like: where a gene's transcription starts"
                                  : c2->cls.find("pELS") != std::string::npos ? "proximal enhancer-like: boosts a nearby gene"
                                  : c2->cls.find("dELS") != std::string::npos ? "distal enhancer-like: can boost genes from afar"
                                  : c2->cls.find("CTCF") != std::string::npos ? "CTCF-bound: helps fold DNA into domains"
                                  : "candidate regulatory element";
                ImGui::TextColored(vec(kMuted), "%s. ENCODE %s, %s", blurb, c2->id.c_str(), spanStr(double(c2->e - c2->s)).c_str());
                ImGui::Unindent(11);
                float y1 = ImGui::GetCursorScreenPos().y - 4;
                cdl->AddRectFilled(ImVec2(cp.x, cp.y + 1), ImVec2(cp.x + 3, y1), U(kColCcre));
            }
        }

        // ---------- variation (one row per ALT allele) ----------
        std::vector<const Variant*> vars;
        for (auto* x : v.variants)
            if (x->s <= p.pos && p.pos < x->e) vars.push_back(x);
        if (!vars.empty()) {
            separator();
            heading("VARIATION  (1KGP, 2,504 people)");
            ImGui::PushFont(fonts().mono);
            for (auto* x : vars) {
                bool common = x->af >= 0.01f;
                float a = common ? 1.f : 0.5f;
                ImVec2 cp = ImGui::GetCursorScreenPos();
                float tx = cp.x;
                auto bases = [&](const std::string& t) {
                    for (char c : t) {
                        char b2[2] = {c, 0};
                        cdl->AddText(ImVec2(tx, cp.y), U(baseCol(c), a), b2);
                        tx += ImGui::CalcTextSize(b2).x;
                    }
                };
                bases(x->ref);
                cdl->AddText(ImVec2(tx + 2, cp.y), U(kMuted, a), ">");
                tx += ImGui::CalcTextSize(">").x + 4;
                bases(x->alt);
                char af[32];
                if (x->af < 0) std::snprintf(af, sizeof af, "AF n/a");
                else if (x->af < 0.001f) std::snprintf(af, sizeof af, "%.3f%%", x->af * 100);
                else std::snprintf(af, sizeof af, "%.2f%%", x->af * 100);
                float barX = cp.x + W - 60;
                ImVec2 as = ImGui::CalcTextSize(af);
                cdl->AddText(ImVec2(barX - 10 - as.x, cp.y), U(kText, a), af);
                float lh = ImGui::GetTextLineHeight();
                cdl->AddRectFilled(ImVec2(barX, cp.y + lh * 0.5f - 3), ImVec2(barX + 60, cp.y + lh * 0.5f + 3), U(kHover), 3);
                // log scale: 0.01% .. 100% over 60 px (clamped below 0.01%), ticks at 0.1%, 1%, 10%
                float fw = 0.f;
                if (x->af > 0) fw = std::max(2.f, 60.f * std::clamp((std::log10(x->af) + 4.f) / 4.f, 0.f, 1.f));
                cdl->AddRectFilled(ImVec2(barX, cp.y + lh * 0.5f - 3), ImVec2(barX + fw, cp.y + lh * 0.5f + 3), U(kAccent, common ? 1.f : 0.4f), 3);
                for (int t = 1; t <= 3; ++t)
                    cdl->AddLine(ImVec2(barX + 15.f * t, cp.y + lh * 0.5f - 5), ImVec2(barX + 15.f * t, cp.y + lh * 0.5f + 5), U(kMuted, 0.7f), 1.f);
                ImGui::Dummy(ImVec2(W, lh + 2));
            }
            ImGui::PopFont();
            if (vars.size() > 1 && vars.front()->alleleCount > 1) {
                ImGui::PushFont(fonts().monoSmall);
                ImGui::TextColored(vec(kMuted, 0.8f), "multi-allelic site: %d ALT alleles", vars.front()->alleleCount);
                ImGui::PopFont();
            }
        }

        // ---------- dbSNP (rsID pack) for the rsID we jumped to ----------
        if (lastRs_.rs && p.pos == lastRs_.pos - 1 && ov_.chroms()[p.ci].name == lastRs_.chrom) {
            separator();
            heading("DBSNP  (common-variant pack)");
            ImGui::PushFont(fonts().mono);
            ImGui::TextColored(vec(kText, 0.95f), "rs%u  %s>%s", lastRs_.rs, lastRs_.ref.c_str(), lastRs_.alt.c_str());
            ImGui::PopFont();
            if (lastRs_.dropped > 0)
                ImGui::TextColored(vec(0x8A8F99), "+%d rarer allele%s listed in dbSNP", lastRs_.dropped, lastRs_.dropped == 1 ? "" : "s");
        }

        // ---------- unknowns ----------
        std::vector<std::string> unknowns;
        for (auto* r : v.repeats) unknowns.push_back("Repeat (" + r->name + ", " + r->cls + "/" + r->family + "), function unknown");
        if (cat == kIntron) unknowns.push_back("Intronic sequence: spliced out, function mostly unknown");
        if (cat == kNothing) unknowns.push_back("No gene, regulatory element or repeat annotated here");
        if (!unknowns.empty()) {
            separator();
            heading("UNKNOWNS");
            for (auto& u : unknowns) {
                swatch(kColFog);
                ImGui::TextColored(vec(kText, 0.9f), "%s", u.c_str());
            }
        }
    }
    if (!pinnedView && pinned_.valid && live) {
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PushFont(fonts().monoSmall);
        ImGui::TextColored(vec(kMuted), "preview - pinned card returns when you move away");
        ImGui::PopFont();
    }
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar(2);
    ImGui::EndChild();
}

// ---------------- settings (key=value, one per line) ----------------
void App::loadSettings() {
    if (opt_.settingsPath.empty()) { introDone_ = true; return; }  // hermetic (shots/scripts): no first-run intro unless --intro
    std::ifstream f(opt_.settingsPath);
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "legend") legendOpen_ = v == "1";
        else if (k == "intro_done") introDone_ = v == "1";
    }
}

void App::saveSettings() const {
    if (opt_.settingsPath.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(opt_.settingsPath).parent_path(), ec);
    std::ofstream f(opt_.settingsPath, std::ios::trunc);
    f << "legend=" << (legendOpen_ ? 1 : 0) << "\n" << "intro_done=" << (introDone_ ? 1 : 0) << "\n";
}

// ---------------- top bar: Legend toggle + settings menu, right of the search box ----------------
void App::drawTopButtons(float w) {
    const float bh = 32, y = (kTopBarH - bh) * 0.5f;
    float x = (w + 320) * 0.5f + 8;  // search box is 320 px, centred
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(fonts().ui);
    const char* lbl = "Legend";
    ImVec2 ts = ImGui::CalcTextSize(lbl);
    ImGui::PushFont(fonts().monoSmall);
    ImVec2 ks = ImGui::CalcTextSize("L");
    ImGui::PopFont();
    float bw = 12 + ts.x + 8 + ks.x + 8 + 10;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ImGui::InvisibleButton("##legendtoggle", ImVec2(bw, bh))) { legendOpen_ = !legendOpen_; saveSettings(); }
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + bh), col(act ? 0x2D3340 : hov ? kHover : kPanel), 6);
    dl->AddRect(ImVec2(x, y), ImVec2(x + bw, y + bh), col(legendOpen_ ? kAccent : kBorder), 6, 0, 1.f);
    dl->AddText(ImVec2(x + 12, y + (bh - ts.y) * 0.5f), col(legendOpen_ ? kAccent : kText), lbl);
    ImGui::PopFont();
    {  // key hint chip
        float kx = x + 12 + ts.x + 8;
        ImVec2 k0(kx, y + 8), k1(kx + ks.x + 8, y + bh - 8);
        dl->AddRect(k0, k1, col(kBorder), 3);
        ImGui::PushFont(fonts().monoSmall);
        dl->AddText(ImVec2(kx + 4, k0.y + (k1.y - k0.y - ks.y) * 0.5f), col(kMuted), "L");
        ImGui::PopFont();
    }
    // settings menu (three dots)
    float sx = x + bw + 6;
    ImGui::SetCursorScreenPos(ImVec2(sx, y));
    if (ImGui::InvisibleButton("##settings", ImVec2(bh, bh))) ImGui::OpenPopup("##settingsmenu");
    hov = ImGui::IsItemHovered();
    dl->AddRectFilled(ImVec2(sx, y), ImVec2(sx + bh, y + bh), col(hov ? kHover : kPanel), 6);
    dl->AddRect(ImVec2(sx, y), ImVec2(sx + bh, y + bh), col(kBorder), 6, 0, 1.f);
    for (int i = -1; i <= 1; ++i) dl->AddCircleFilled(ImVec2(sx + bh * 0.5f + i * 5.f, y + bh * 0.5f), 1.6f, col(kMuted));
    ImGui::SetNextWindowPos(ImVec2(sx, y + bh + 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    if (ImGui::BeginPopup("##settingsmenu")) {
        ImGui::PushFont(fonts().monoSmall);
        ImGui::TextColored(vec(kMuted), "SETTINGS");
        ImGui::PopFont();
        if (ImGui::MenuItem("Legend", "L", legendOpen_)) { legendOpen_ = !legendOpen_; saveSettings(); }
        if (ImGui::MenuItem("Show intro again")) startIntro();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// ---------------- legend panel ----------------
void App::drawLegend(ImVec2 A, ImVec2 B, FogRenderer& fog) {
    ImDrawList* rdl = ImGui::GetWindowDrawList();
    rdl->AddRectFilled(A, B, col(kPanel), 8);  // #171A21
    rdl->AddRect(A, B, col(kBorder), 8);
    // pinned header: 'Legend' + 'L to close' stay put; only the list below scrolls
    ImGui::PushFont(fonts().uiBold);
    const float th = ImGui::GetTextLineHeight();
    rdl->AddText(ImVec2(A.x + 12, A.y + 12), col(kText), "Legend");
    ImGui::PopFont();
    ImGui::PushFont(fonts().monoSmall);
    const char* closeHint = opt_.phone ? "swipe down to close" : "L to close";
    ImVec2 hs = ImGui::CalcTextSize(closeHint);
    rdl->AddText(ImVec2(B.x - 12 - hs.x, A.y + 12 + (th - hs.y) * 0.5f), col(kMuted), closeHint);
    ImGui::PopFont();
    const float headH = 12 + th + 8;
    rdl->AddLine(ImVec2(A.x + 1, A.y + headH), ImVec2(B.x - 1, A.y + headH), col(kBorder));
    ImGui::SetCursorScreenPos(ImVec2(A.x, A.y + headH + 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(kPanel, 0.f));
    ImGui::BeginChild("##legend", ImVec2(B.x - A.x, B.y - A.y - headH - 2), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    if (opt_.phone && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.f))
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);  // touch scroll
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = ImGui::GetCursorScreenPos().x;

    using Swatch = std::function<void(ImDrawList*, float, float, float, float)>;  // x0, y0, x1, y1 of the 16x10 box
    struct Hdr { const char* name; float y; };  // header text top, screen space at scroll 0
    std::vector<Hdr> hdrs;
    const float scrollY = ImGui::GetScrollY();
    auto group = [&](const char* name) {
        ImGui::Dummy(ImVec2(1, 4));
        hdrs.push_back({name, ImGui::GetCursorScreenPos().y + scrollY});
        ImGui::PushFont(fonts().monoSmall);
        ImGui::TextColored(vec(0x6B7280), "%s", name);
        ImGui::PopFont();
    };
    auto row = [&](const char* label, const char* meaning, const Swatch& sw) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushFont(fonts().ui13);
        float lh = ImGui::GetTextLineHeight();
        float sy = p.y + (lh - 10) * 0.5f;
        dl->PushClipRect(ImVec2(x0, sy - 6), ImVec2(x0 + 16, sy + 10), true);  // carets may poke above the box
        sw(dl, x0, sy, x0 + 16, sy + 10);
        dl->PopClipRect();
        dl->AddText(ImVec2(x0 + 24, p.y), col(kText), label);
        ImGui::PopFont();
        ImGui::PushFont(fonts().uiSmall);
        dl->AddText(ImVec2(x0 + 24, p.y + lh + 1), col(0x8A8F99), meaning);
        float mh = ImGui::GetTextLineHeight();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(B.x - A.x - 24, lh + 1 + mh + 4));
    };
    auto solid = [](unsigned c) { return [c](ImDrawList* d, float a, float b, float x1, float y1) { d->AddRectFilled(ImVec2(a, b), ImVec2(x1, y1), col(c)); }; };
    auto fogSw = [&](float alpha, float edge, unsigned under) {
        return [&fog, alpha, edge, under](ImDrawList* d, float a, float b, float x1, float y1) {
            if (under) d->AddRectFilled(ImVec2(a, b), ImVec2(x1, y1), col(under));
            fog.draw(d, {FogRect{a, b, x1, y1, alpha, edge, edge}}, 24.f);
        };
    };

    group("COVERAGE");  // category strip colours (catColor) + nothing-known fog
    row("Exon", "Gene exons: known function", solid(catColor(kExon)));
    row("Regulatory (cCRE)", "ENCODE candidate regulatory element", solid(catColor(kCcre)));
    row("Intron", "Inside a gene, spliced out of the RNA", solid(catColor(kIntron)));
    row("Repeat", "Known repeat family, function unclear", solid(catColor(kRepeat)));
    row("Nothing known", "No annotation; drifting fog marks it", fogSw(0.55f, 3.f, catColor(kNothing)));

    group("GENES");
    row("Gene model", "Exons as blocks on the intron line", [](ImDrawList* d, float a, float b, float x1, float y1) {
        geneBand(d, a, x1, b + 5);
        exonBlock(d, a + 3, a + 8, b, y1);
    });
    row("Strand", "Chevrons point the way it is read", [](ImDrawList* d, float a, float b, float x1, float) {
        geneBand(d, a, x1, b + 5);
        strandChevron(d, a + 9, b + 5, 1.f);
    });
    row("Search match", "Outline fades out after a jump", [](ImDrawList* d, float a, float b, float x1, float y1) {
        geneBand(d, a + 2, x1 - 2, b + 5);
        d->AddRect(ImVec2(a + 0.5f, b + 0.5f), ImVec2(x1 - 0.5f, y1 - 0.5f), col(kAccent), 2, 0, 1.5f);
    });

    group("REGULATORY");
    row("cCRE", "Regulatory element (ENCODE)", solid(kColCcre));

    group("REPEATS");
    row("Repeat", "RepeatMasker element", solid(kColRepeat));

    if (store_.hasVariants()) {
        group("VARIANTS");
        row("Common variant", "Allele frequency 1% or more (1KGP)", [](ImDrawList* d, float a, float b, float, float y1) {
            for (int i = 0; i < 4; ++i) variantTick(d, a + 2 + i * 4, a + 3 + i * 4, b, y1, 0.2f);
        });
        row("Rare variant", "Under 1%, drawn dimmer", [](ImDrawList* d, float a, float b, float, float y1) {
            for (int i = 0; i < 4; ++i) variantTick(d, a + 2 + i * 4, a + 3 + i * 4, b, y1, 0.001f);
        });
        row("Density", "Zoomed out: bar height = count", [](ImDrawList* d, float a, float b, float, float y1) {
            const float h[8] = {3, 7, 10, 5, 2, 8, 4, 6};
            for (int i = 0; i < 8; ++i)
                d->AddRectFilled(ImVec2(a + i * 2, y1 - h[i]), ImVec2(a + i * 2 + 1, y1), col(kAccent, variantAlpha(i % 3 ? 0.2f : 0.001f)));
        });
    }
    if (store_.hasSv()) {
        group("SVS");
        row("Insertion", "Sequence added at this point", [](ImDrawList* d, float a, float b, float, float y1) { svIns(d, a + 8, 0.2f, b + 3, y1, false); });
        row("Deletion", "Sequence missing in some genomes", [](ImDrawList* d, float a, float b, float x1, float y1) {
            svBar(d, SvType::Del, false, 0.2f, a, x1, b + 1, y1 - 1, false, 1.f, a, x1);
        });
        row("Complex", "Rearranged, several changes at once", [](ImDrawList* d, float a, float b, float x1, float y1) {
            svBar(d, SvType::Complex, false, 0.2f, a, x1, b + 1, y1 - 1, false, 1.f, a, x1);
        });
        row("Contains an inversion", "Violet top edge on a complex SV", [](ImDrawList* d, float a, float b, float x1, float y1) {
            svBar(d, SvType::Complex, true, 0.2f, a, x1, b + 1, y1 - 1, false, 1.f, a, x1);
            d->AddRectFilled(ImVec2(a, b + 1), ImVec2(x1, b + 2), col(kSvInvEdge));
        });
        row("Rare SV", "Allele frequency under 1%, faded", [](ImDrawList* d, float a, float b, float x1, float y1) {
            svBar(d, SvType::Del, false, 0.001f, a, a + 9, b + 1, y1 - 1, false, 1.f, a, x1);
            svIns(d, a + 13, 0.001f, b + 1, y1, false);
        });
    }
    if (store_.hasMethyl()) {
        group("METHYLATION");
        row("CpG methylation", "Dark = unmethylated, violet = methylated", [](ImDrawList* d, float a, float b, float, float y1) {
            for (int i = 0; i < 16; ++i) d->AddRectFilled(ImVec2(a + i, b), ImVec2(a + i + 1, y1), methylCol(i / 15.f));
        });
    }

    group("OVERLAYS");
    row(opt_.phone ? "New in T2T" : "Not in GRCh38", opt_.phone ? "Not in GRCh38; most annotations miss it" : "New in T2T; most annotations miss it", [](ImDrawList* d, float a, float b, float x1, float y1) {
        notInFill(d, a, b, x1, y1);
        hatch(d, a, b, x1, y1);
    });
    row("Loading", "Fog while a track is still loading", fogSw(0.40f, 3.f, kHover));
    row("Hover", "Position under the cursor", [](ImDrawList* d, float a, float b, float, float y1) { hoverCursor(d, a + 8, b, y1); });
    row("Pinned", "Position whose card is pinned", [](ImDrawList* d, float a, float b, float, float y1) { pinMarker(d, a + 8, b + 12, y1 - 4); });

    // sticky group header: the group whose header has scrolled past the top stays pinned there (opaque band),
    // pushed up by the next header as it arrives - same behaviour as the search list's sticky group rows
    if (scrollY > 0 && !hdrs.empty()) {
        ImGui::PushFont(fonts().monoSmall);
        const float hh = ImGui::GetTextLineHeight();
        ImGui::PopFont();
        const float top = ImGui::GetWindowPos().y;  // the child's top edge, just under the pinned header
        const float pad = 6.f, band = hh + 2 * pad;
        int cur = -1;
        for (int i = 0; i < int(hdrs.size()); ++i)
            if (hdrs[i].y - scrollY <= top + pad) cur = i;
        if (cur >= 0) {
            float y = top;
            if (cur + 1 < int(hdrs.size())) y = std::min(y, hdrs[cur + 1].y - scrollY - pad - band);
            ImGui::PushClipRect(ImVec2(A.x + 1, A.y + 1), ImVec2(B.x - 1, B.y - 1), false);
            dl->PushClipRect(ImVec2(A.x + 1, A.y + 1), ImVec2(B.x - 7, B.y - 1), false);  // keep the scrollbar visible
            dl->AddRectFilled(ImVec2(A.x + 1, y), ImVec2(B.x - 1, y + band), col(kPanel), 0.f);
            dl->AddLine(ImVec2(A.x + 1, y + band), ImVec2(B.x - 1, y + band), col(kBorder));
            ImGui::PushFont(fonts().monoSmall);
            dl->AddText(ImVec2(x0, y + pad), col(0x6B7280), hdrs[cur].name);
            ImGui::PopFont();
            dl->PopClipRect();
            ImGui::PopClipRect();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// ---------------- first-run intro ----------------
void App::startIntro() {
    introStep_ = 0;
    introT0_ = clock_;
    searchOpen_ = false;
}

void App::endIntro() {
    introStep_ = -1;
    introDone_ = true;
    saveSettings();
}

void App::drawIntro(ImVec2 D) {
    if (introStep_ < 0) return;
    struct Step { ImVec2 a, b; const char* title; const char* body; bool above; };
    const Step steps[3] = {
        {ovA_, ovB_, "Overview", "Each strip is one chromosome. Brighter means more is known there.", false},
        {rgA_, rgB_, "Region view", "Scroll to zoom, drag to pan. Click anything to pin its card.", true},
        {searchA_, searchB_, "Search", "Search a gene like COMT, or a position.", false},
    };
    const Step& st = steps[introStep_];
    const float fade = opt_.reducedMotion ? 1.f : float(std::clamp((clock_ - introT0_) / 0.15, 0.0, 1.0));
    // Scrim + ring live in their own input-less overlay window, above the root window and all of its children
    // (legend, card, search border); only the intro card window (focused below) sits above it.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(D);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##introScrim", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar();
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRectFullScreen();
    ImVec2 r0(st.a.x - 4, st.a.y - 4), r1(st.b.x + 4, st.b.y + 4);  // ring sits 4 px outside the target
    const ImU32 scrim = col(kBg, 0.50f);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(D.x, r0.y), scrim);
    dl->AddRectFilled(ImVec2(0, r1.y), D, scrim);
    dl->AddRectFilled(ImVec2(0, r0.y), ImVec2(r0.x, r1.y), scrim);
    dl->AddRectFilled(ImVec2(r1.x, r0.y), ImVec2(D.x, r1.y), scrim);
    dl->AddRect(r0, r1, col(kAccent, fade), 8, 0, 2.f);
    dl->PopClipRect();
    ImGui::End();

    const float cw = 360, arrow = 12;
    float cx = std::clamp((r0.x + r1.x) * 0.5f - cw * 0.5f, kMargin, D.x - kMargin - cw);
    float cy = st.above ? r0.y - arrow - introCardH_ : r1.y + arrow;
    float ax = std::clamp((r0.x + r1.x) * 0.5f, cx + 24, cx + cw - 24);
    ImGui::SetNextWindowPos(ImVec2(cx, cy));
    ImGui::SetNextWindowSize(ImVec2(cw, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(kPanel));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(kBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    ImGui::SetNextWindowFocus();
    ImGui::Begin("##intro", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_AlwaysAutoResize);
    ImDrawList* wdl = ImGui::GetWindowDrawList();
    {  // 12 px arrow from the card edge to the target
        wdl->PushClipRectFullScreen();
        ImU32 fill = col(kPanel, fade), edge = col(kBorder, fade);
        if (st.above) {
            float by = cy + introCardH_ - 1;
            wdl->AddTriangleFilled(ImVec2(ax - 10, by), ImVec2(ax + 10, by), ImVec2(ax, by + arrow), fill);
            wdl->AddLine(ImVec2(ax - 10, by), ImVec2(ax, by + arrow), edge);
            wdl->AddLine(ImVec2(ax, by + arrow), ImVec2(ax + 10, by), edge);
        } else {
            float by = cy + 1;
            wdl->AddTriangleFilled(ImVec2(ax - 10, by), ImVec2(ax, by - arrow), ImVec2(ax + 10, by), fill);
            wdl->AddLine(ImVec2(ax - 10, by), ImVec2(ax, by - arrow), edge);
            wdl->AddLine(ImVec2(ax, by - arrow), ImVec2(ax + 10, by), edge);
        }
        wdl->PopClipRect();
    }
    ImGui::PushFont(fonts().monoSmall);
    ImGui::TextColored(vec(kMuted), "STEP %d OF 3", introStep_ + 1);
    ImGui::PopFont();
    ImGui::PushFont(fonts().uiBold);
    ImGui::TextColored(vec(kText), "%s", st.title);
    ImGui::PopFont();
    ImGui::PushTextWrapPos(cw - 16);
    ImGui::TextColored(vec(kText, 0.85f), "%s", st.body);
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(1, 6));
    bool next = false, skip = false;
    const bool last = introStep_ == 2;
    if (!last) {
        if (ImGui::Button("Skip", ImVec2(80, 30))) skip = true;
        ImGui::SameLine(cw - 32 - 100);
    } else {
        ImGui::SetCursorPosX(cw - 16 - 100);
    }
    ImGui::PushStyleColor(ImGuiCol_Button, vec(kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec(0x9AD3FF));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, vec(0x9AD3FF));
    ImGui::PushStyleColor(ImGuiCol_Text, vec(kBg));
    if (ImGui::Button(last ? "Done" : "Next", ImVec2(100, 30))) next = true;
    ImGui::PopStyleColor(4);
    {  // step dots
        ImGui::Dummy(ImVec2(1, 4));
        ImVec2 p = ImGui::GetCursorScreenPos();
        float mid = cx + cw * 0.5f;
        for (int i = 0; i < 3; ++i)
            wdl->AddCircleFilled(ImVec2(mid + (i - 1) * 14.f, p.y + 3), 3.f, col(i == introStep_ ? kAccent : kBorder, fade));
        ImGui::Dummy(ImVec2(1, 6));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) next = true;
    introCardH_ = ImGui::GetWindowHeight();
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    if (skip) endIntro();
    else if (next) {
        if (last) endIntro();
        else { ++introStep_; introT0_ = clock_; }  // reducedMotion: fade is 1 immediately (no fade between steps)
    }
}


// =====================================================================================================================
// Phone layout (portrait, sizes in dp; Android always, desktop with --phone). Designer spec:
//   48 dp top bar (logo, search icon -> full-screen search, menu) | 64 dp chromosome chip picker with mini bars |
//   region view fills the rest (56 dp label gutter, tracks x1.5, hints hidden; pinch zoom, one-finger pan, tap pins,
//   long-press previews) | card as a bottom sheet (96 dp peek -> half -> full, swipe down unpins) | legend sheet from
//   the menu | 44 dp minimum touch targets.
// =====================================================================================================================
namespace {
constexpr float kPhoneTopH = 48.f, kPhoneChipH = 64.f, kSheetPeek = 96.f, kSheetHandle = 44.f;
const char* chipName(const std::string& n) { return n.rfind("chr", 0) == 0 ? n.c_str() + 3 : n.c_str(); }
struct PackInfo { const char* id; const char* name; const char* desc; };
const PackInfo kPackInfo[] = {
    {"core", "Core tracks", "Genes, ENCODE regulatory elements, repeats, regions new in T2T and the genome overview. Required."},
    {"sv", "Structural variants", "HPRC pangenome SVs (50 bp and up), shown below 3 Mb."},
    {"methyl", "CpG methylation", "CHM13 nanopore methylation strip under the regulatory track."},
    {"genome", "Genome sequence", "hs1 bases, shown when zoomed below 20 kb."},
    {"rsid", "Common variants (rsID search)", "19.4 million dbSNP 155 rsIDs for the search box."},
};
std::string mbStr(long long b) {
    char t[32];
    if (b >= 1024LL * 1048576) std::snprintf(t, sizeof t, "%.1f GB", b / 1073741824.0);
    else std::snprintf(t, sizeof t, "%.0f MB", std::max(1.0, b / 1048576.0));
    return t;
}
// handle bar + 44 dp drag zone at the top of a sheet; returns the drag delta on release (0 while dragging), tap = 0.001
float sheetHandle(const char* id, ImDrawList* dl, float top, float w, float& live) {
    dl->AddRectFilled(ImVec2(w * 0.5f - 18, top + 8), ImVec2(w * 0.5f + 18, top + 12), col(kBorder), 2);
    ImGui::SetCursorScreenPos(ImVec2(0, top));
    ImGui::InvisibleButton(id, ImVec2(w, kSheetHandle));
    if (ImGui::IsItemActive()) live = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).y;
    if (ImGui::IsItemDeactivated()) {
        float d = live;
        live = 0;
        return std::fabs(d) < 10.f ? 0.001f : d;
    }
    return 0.f;
}
}  // namespace

void App::onFinger(int64_t id, float x, float y, int type) {
    if (type == 2) fingers_.erase(id);
    else fingers_[id] = ImVec2(x, y);
}

long long App::shownDlBytes(int& phase) const {
    long long b = dlDoneBytes_ + dlCurBytes_, t = dlTotal_;
    phase = dlPhase_;
    if (virtualClock_ >= 0) {  // script mode: progress from the virtual clock (20 MB/s), not wall time
        b = std::min<long long>(t, (long long)((clock_ - dlT0_) * 20000.0 * 1024.0));
        phase = b >= t ? 1 : 0;
    }
    return b;
}

bool App::dlHeld() const {
    if (virtualClock_ < 0 || dlState_ != 1) return false;
    int ph = 0;
    return shownDlBytes(ph) >= dlTotal_;
}

void App::phonePinch() {
    if (fingers_.size() < 2) { pinching_ = false; return; }
    auto it = fingers_.begin();
    ImVec2 p1 = it->second, p2 = (++it)->second;
    float d = std::max(1.f, std::hypot(p2.x - p1.x, p2.y - p1.y));
    float cx = (p1.x + p2.x) * 0.5f;
    double span = viewE_ - viewS_;
    if (!pinching_) {
        pinching_ = true;
        pinchD0_ = d;
        pinchS0_ = viewS_;
        pinchE0_ = viewE_;
        pinchBp_ = viewS_ + (cx - regionTx0_) * span / std::max(1.f, regionWidthPx_);
        animating_ = false;
    }
    double ns = (pinchE0_ - pinchS0_) * double(pinchD0_ / d);
    double s = pinchBp_ - (cx - regionTx0_) * ns / std::max(1.f, regionWidthPx_);  // the base under the fingers stays put
    setView(viewCi_, s, s + ns);
}

void App::drawPhoneTopBar(ImDrawList* dl, float w) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, kPhoneTopH), col(kPanel));
    dl->AddLine(ImVec2(0, kPhoneTopH - 0.5f), ImVec2(w, kPhoneTopH - 0.5f), col(kBorder));
    ImVec2 c(16 + 8, kPhoneTopH * 0.5f);
    dl->AddCircleFilled(c, 8, col(kAccent, 0.18f));
    dl->AddCircleFilled(c, 4, col(kAccent));
    ImGui::PushFont(fonts().title);
    float th = ImGui::GetTextLineHeight();
    dl->AddText(ImVec2(40, (kPhoneTopH - th) * 0.5f), col(kText), "ProjectWisp");
    ImGui::PopFont();
    auto iconBtn = [&](const char* id, float x, bool on) {
        ImGui::SetCursorScreenPos(ImVec2(x, 2));
        bool hit = ImGui::InvisibleButton(id, ImVec2(44, 44));
        if (ImGui::IsItemActive() || on) dl->AddRectFilled(ImVec2(x + 2, 4), ImVec2(x + 42, 44), col(kHover), 8);
        return hit;
    };
    float sx = w - 96;
    if (iconBtn("##psearch", sx, phoneSearch_)) { phoneSearch_ = true; searchFocus_ = true; phoneMenu_ = false; }
    ImVec2 sc(sx + 20, 22);  // magnifier
    dl->AddCircle(sc, 7, col(kText), 0, 1.8f);
    dl->AddLine(ImVec2(sc.x + 5, sc.y + 5), ImVec2(sc.x + 11, sc.y + 11), col(kText), 2.f);
    float mx = w - 48;
    if (iconBtn("##pmenu", mx, phoneMenu_)) phoneMenu_ = !phoneMenu_;
    for (int i = -1; i <= 1; ++i) dl->AddCircleFilled(ImVec2(mx + 22 + i * 6.f, 24), 2.f, col(kText));
}

void App::drawChips(ImDrawList* dl, ImVec2 A, ImVec2 B) {
    ImGuiIO& io = ImGui::GetIO();
    const float chipW = 48, gap = 6, x0 = 8;
    const int n = int(order_.size());
    const float total = x0 * 2 + n * chipW + (n - 1) * gap, maxScroll = std::max(0.f, total - (B.x - A.x));
    if (lastChipCi_ != viewCi_) {  // keep the selected chip in view
        for (int r = 0; r < n; ++r)
            if (order_[r] == viewCi_) {
                float cx = x0 + r * (chipW + gap);
                if (cx - chipScroll_ < 8) chipScroll_ = cx - 8;
                if (cx + chipW - chipScroll_ > B.x - A.x - 8) chipScroll_ = cx + chipW - (B.x - A.x) + 8;
            }
        lastChipCi_ = viewCi_;
    }
    ImGui::SetCursorScreenPos(A);
    ImGui::InvisibleButton("##chips", ImVec2(B.x - A.x, B.y - A.y));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 8.f)) chipScroll_ -= io.MouseDelta.x;
    chipScroll_ = std::clamp(chipScroll_, 0.f, maxScroll);
    int tapped = -1;
    dl->AddRectFilled(A, B, col(kBg));
    dl->PushClipRect(A, B, true);
    for (int r = 0; r < n; ++r) {
        int ci = order_[r];
        const auto& ch = ov_.chroms()[ci];
        float x = A.x + x0 + r * (chipW + gap) - chipScroll_;
        if (x > B.x || x + chipW < A.x) continue;
        ImVec2 c0(x, A.y + 8), c1(x + chipW, B.y - 8);
        bool sel = ci == viewCi_;
        if (ImGui::IsItemDeactivated() && io.MouseDragMaxDistanceSqr[0] < 100.f && io.MousePos.x >= c0.x && io.MousePos.x < c1.x) tapped = ci;
        dl->AddRectFilled(c0, c1, col(sel ? kHover : kPanel), 8);
        dl->AddRect(c0, c1, col(sel ? kAccent : kBorder), 8, 0, sel ? 1.5f : 1.f);
        ImGui::PushFont(fonts().uiBold);
        const char* nm = chipName(ch.name);
        ImVec2 ts = ImGui::CalcTextSize(nm);
        dl->AddText(ImVec2(x + (chipW - ts.x) * 0.5f, c0.y + 6), col(sel ? kAccent : kText), nm);
        ImGui::PopFont();
        // mini bar: known | introns+repeats | nothing known, not-in-GRCh38 hatched over the right end
        float mw = chipW - 12, mx = x + 6, my0 = c1.y - 12, my1 = c1.y - 7, bx = mx;
        double sh[3] = {ch.share(kExon) + ch.share(kCcre), ch.share(kIntron) + ch.share(kRepeat), ch.share(kNothing)};
        unsigned cc[3] = {kColKnown, kColIntron, kColFog};
        double tot = sh[0] + sh[1] + sh[2];
        for (int k = 0; k < 3; ++k) {
            float ww = tot > 0 ? float(mw * sh[k] / tot) : 0.f;
            dl->AddRectFilled(ImVec2(bx, my0), ImVec2(bx + ww, my1), col(cc[k]));
            bx += ww;
        }
        float hw = float(mw * std::clamp(ch.share(kNotIn), 0.0, 1.0));
        if (ch.share(kNotIn) >= 0.025) {
            dl->AddRectFilled(ImVec2(mx + mw - hw, my0), ImVec2(mx + mw, my1), col(kBg, 0.35f));
            hatch(dl, mx + mw - hw, my0, mx + mw, my1, 1.f);
        }
    }
    dl->PopClipRect();
    if (tapped >= 0 && tapped != viewCi_) { setView(tapped, 0, 1e18); animating_ = false; }
}

void App::drawCardSheet(ImDrawList* dl, ImVec2 D) {
    const bool live = hover_.valid;
    if (!pinned_.valid && !live) { sheetState_ = 0; return; }
    const float hs[3] = {kSheetPeek, D.y * 0.5f, D.y - kPhoneTopH - 8};
    int st = live ? 0 : sheetState_;
    float top = std::clamp(D.y - hs[st] + sheetDragY_, kPhoneTopH + 8, D.y - 40);
    ImDrawList* sdl = ImGui::GetWindowDrawList();
    sdl->AddRectFilled(ImVec2(0, top), ImVec2(D.x, D.y), col(kPanel), 12, ImDrawFlags_RoundCornersTop);
    sdl->AddLine(ImVec2(12, top), ImVec2(D.x - 12, top), col(live ? kBorder : kAccent), 1.f);
    drawCard(dl, ImVec2(0, top + kSheetHandle - 16), ImVec2(D.x, D.y + 16));  // card pads 16: content starts under the handle
    if (live) return;  // long-press preview: no sheet gestures
    float d = sheetHandle("##cardsheet", sdl, top, D.x, sheetDragY_);
    if (d == 0.001f) sheetState_ = sheetState_ == 0 ? 1 : 0;
    else if (d < -40.f) sheetState_ = std::min(2, sheetState_ + (d < -D.y * 0.4f ? 2 : 1));
    else if (d > 40.f) {
        if (sheetState_ == 0 || d > D.y * 0.4f) { pinned_ = {}; sheetState_ = 0; }  // swipe down from the peek unpins
        else --sheetState_;
    }
}

void App::drawLegendSheet(ImVec2 D, FogRenderer& fog) {
    float h = D.y * 0.62f;
    float top = std::max(kPhoneTopH + 8, D.y - h + std::max(0.f, sheetDragY_));
    drawLegend(ImVec2(0, top), ImVec2(D.x, D.y + 10), fog);
    float d = sheetHandle("##legendsheet", ImGui::GetWindowDrawList(), top, D.x, sheetDragY_);
    if (d > 40.f) { legendOpen_ = false; sheetDragY_ = 0; }
}

void App::drawPhoneMenu(ImVec2 D) {
    const float w = 228;
    ImGui::SetNextWindowPos(ImVec2(D.x - w - 8, kPhoneTopH + 4));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(kPanel));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(kBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
    ImGui::Begin("##phonemenu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                             ImGuiWindowFlags_AlwaysAutoResize);
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    auto item = [&](const char* label) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12);
        return ImGui::Selectable(label, false, 0, ImVec2(w - 32, 48));
    };
    if (item("Legend")) { legendOpen_ = true; sheetDragY_ = 0; phoneMenu_ = false; }
    if (item("Downloads")) { phoneDownloads_ = true; phoneMenu_ = false; }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    if (ImGui::IsMouseClicked(0) && !ImGui::IsMouseHoveringRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), false) &&
        !ImGui::IsMouseHoveringRect(ImVec2(D.x - 48, 0), ImVec2(D.x, kPhoneTopH), false))
        phoneMenu_ = false;  // tap outside closes
}

void App::drawDataScreen(ImVec2 D, bool firstRun) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), D, col(kBg));
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(D.x, kPhoneTopH), col(kPanel));
    dl->AddLine(ImVec2(0, kPhoneTopH - 0.5f), ImVec2(D.x, kPhoneTopH - 0.5f), col(kBorder));
    float tx = 16;
    if (!firstRun) {  // back arrow
        ImGui::SetCursorScreenPos(ImVec2(2, 2));
        if (ImGui::InvisibleButton("##dlback", ImVec2(44, 44))) phoneDownloads_ = false;
        dl->AddLine(ImVec2(16, 24), ImVec2(30, 24), col(kText), 2.f);
        dl->AddLine(ImVec2(16, 24), ImVec2(22, 18), col(kText), 2.f);
        dl->AddLine(ImVec2(16, 24), ImVec2(22, 30), col(kText), 2.f);
        tx = 52;
    }
    ImGui::PushFont(fonts().title);
    dl->AddText(ImVec2(tx, (kPhoneTopH - ImGui::GetTextLineHeight()) * 0.5f), col(kText), firstRun ? "Get the data" : "Downloads");
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(ImVec2(0, kPhoneTopH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4);
    ImGui::BeginChild("##datascreen", ImVec2(D.x, D.y - kPhoneTopH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.f))
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
    const float W = ImGui::GetContentRegionAvail().x - 2;
    ImGui::PushTextWrapPos(W + 16);
    if (firstRun)
        ImGui::TextColored(vec(kMuted), "ProjectWisp shows the T2T-CHM13 (hs1) genome. It needs the core tracks before it can draw "
                                        "anything; the rest is optional. Files come from the project's GitHub data release and are "
                                        "checked with SHA-256.");
    else
        ImGui::TextColored(vec(kMuted), "Optional data for this phone. Files come from the project's GitHub data release and are "
                                        "checked with SHA-256.");
    ImGui::Dummy(ImVec2(0, 4));
    long long all = 0;
    for (const PackInfo& pi : kPackInfo) all += packBytes(pi.id);
    ImGui::PushFont(fonts().mono);
    ImGui::TextColored(vec(kText), "Core %s, everything %s", mbStr(packBytes("core")).c_str(), mbStr(all).c_str());
    ImGui::PopFont();
    ImGui::TextColored(vec(0x8A8F99), "%s", kStorageNote);  // shown right after the total size
    ImGui::Dummy(ImVec2(0, 4));
    auto outlined = [&](const char* label, ImVec2 sz) {  // secondary button: outline, #E6E8EC text
        ImGui::PushStyleColor(ImGuiCol_Button, vec(kBg, 0.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec(kHover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, vec(0x2D3340));
        ImGui::PushStyleColor(ImGuiCol_Border, vec(kMuted));
        ImGui::PushStyleColor(ImGuiCol_Text, vec(kText));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        bool hit = ImGui::Button(label, sz);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
        return hit;
    };
    {
        std::vector<std::string> missing;
        for (const PackInfo& pi : kPackInfo)
            if (packBytes(pi.id) && !packInstalled(pi.id) && !(std::string(pi.id) == "rsid" && rsid_.present())) missing.push_back(pi.id);
        std::string lbl = "Download all (" + mbStr(all) + ")";
        ImGui::BeginDisabled(dlState_ == 1 || missing.empty() || !dlQueue_.empty());
        if (outlined(lbl.c_str(), ImVec2(W, 44)) && !missing.empty()) {
            dlQueue_.assign(missing.begin() + 1, missing.end());
            startDownload(missing.front());
        }
        ImGui::EndDisabled();
        ImGui::PushFont(fonts().ui13);
        ImGui::TextColored(vec(kMuted), "Use Wi-Fi for big packs.");
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(0, 8));
    const bool coreOk = packInstalled("core");
    const int st = dlState_;
    for (const PackInfo& pi : kPackInfo) {
        long long bytes = packBytes(pi.id);
        if (!bytes) continue;
        bool installed = packInstalled(pi.id) || (std::string(pi.id) == "rsid" && rsid_.present());
        bool mine = dlPack_ == pi.id;
        ImGui::PushID(pi.id);
        ImVec2 c0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Indent(12);
        ImGui::PushFont(fonts().uiBold);
        ImGui::TextUnformatted(pi.name);
        ImGui::PopFont();
        std::string sz = mbStr(bytes);
        ImGui::SameLine(W - 12 - ImGui::CalcTextSize(sz.c_str()).x);
        ImGui::PushFont(fonts().mono);
        ImGui::TextColored(vec(kText), "%s", sz.c_str());
        ImGui::PopFont();
        ImGui::PushTextWrapPos(c0.x + W - 12);
        ImGui::PushFont(fonts().ui13);
        ImGui::TextColored(vec(kMuted), "%s", pi.desc);
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
        if (installed) {
            ImGui::TextColored(vec(kAccent), "Installed");
        } else if (mine && st == 1) {
            int ph = 0;
            long long b = shownDlBytes(ph), t = dlTotal_;
            char over[96];
            if (ph == 0) std::snprintf(over, sizeof over, "Downloading %.0f / %.0f MB", b / 1048576.0, t / 1048576.0);
            else std::snprintf(over, sizeof over, "Verifying checksum...");
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, vec(kAccent));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, vec(kHover));
            ImGui::ProgressBar(ph == 0 && t > 0 ? float(double(b) / double(t)) : 1.f, ImVec2(W - 24, 28), over);
            ImGui::PopStyleColor(2);
        } else {
            if (mine && st == 3) {
                std::lock_guard<std::mutex> l(dlMu_);
                ImGui::PushTextWrapPos(c0.x + W - 12);
                ImGui::TextColored(vec(0xE8A13A), "Failed: %s", dlMsg_.c_str());
                ImGui::PopTextWrapPos();
            }
            bool primary = std::string(pi.id) == "core";
            bool can = st != 1 && (primary || coreOk);
            ImGui::BeginDisabled(!can);
            if (primary) {
                ImGui::PushStyleColor(ImGuiCol_Button, vec(kAccent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec(0x9AD3FF));
                ImGui::PushStyleColor(ImGuiCol_Text, vec(kBg));
            }
            const char* bl = mine && st == 3 ? "Retry" : "Download";
            if (primary) {
                if (ImGui::Button(bl, ImVec2(140, 44))) startDownload(pi.id);
                ImGui::PopStyleColor(3);
            } else if (outlined(bl, ImVec2(140, 44))) startDownload(pi.id);
            ImGui::EndDisabled();
        }
        ImGui::Unindent(12);
        ImGui::Dummy(ImVec2(0, 6));
        ImVec2 c1(c0.x + W, ImGui::GetCursorScreenPos().y);
        ImGui::GetWindowDrawList()->AddRect(c0, c1, col(mine && st == 1 ? kAccent : kBorder), 8);
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PopID();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

void App::framePhone(FogRenderer& fog) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 D = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(D);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), D, col(kBg));
    hover_ = {};
    if (virtualClock_ >= 0) clock_ = virtualClock_;
    else clock_ += io.DeltaTime;
    int vph = 0;
    const bool landed = dlState_ == 2 && (virtualClock_ < 0 || shownDlBytes(vph) >= dlTotal_);  // script: on the virtual clock
    if (landed && !dlQueue_.empty()) {  // 'Download all': next pack
        if (needData_ && dlPack_ == "core") {}  // load the map first (below), the queue continues next frame
        else {
            if (dlPack_ == "rsid" && !rsid_.present()) rsid_.open(opt_.rsidPack.empty() ? opt_.dataDir + "/packs/rsid/rsid.idx" : opt_.rsidPack);
            std::string next = dlQueue_.front();
            dlQueue_.erase(dlQueue_.begin());
            startDownload(next);
        }
    }
    if (dlState_ == 3) dlQueue_.clear();
    if (needData_ || phoneDownloads_) {
        if (needData_ && landed && dlPack_ == "core") {  // core tracks arrived: load them and show the map
            std::string err;
            if (!initData(err)) {
                std::lock_guard<std::mutex> l(dlMu_);
                dlMsg_ = err;
                dlState_ = 3;
                needData_ = true;
            }
        }
        if (dlState_ == 2 && dlPack_ == "rsid" && !rsid_.present()) rsid_.open(opt_.rsidPack.empty() ? opt_.dataDir + "/packs/rsid/rsid.idx" : opt_.rsidPack);
        if (needData_ || phoneDownloads_) {
            if (!needData_ && (ImGui::IsKeyPressed(ImGuiKey_AppBack) || ImGui::IsKeyPressed(ImGuiKey_Escape))) phoneDownloads_ = false;
            drawDataScreen(D, needData_);
            ImGui::End();
            return;
        }
    }
    if (animating_) {
        double t = std::clamp((clock_ - animT0_) / 0.3, 0.0, 1.0);
        double k = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
        double s0 = animS0_ + (animS1_ - animS0_) * k, e0 = animE0_ + (animE1_ - animE0_) * k;
        if (t >= 1.0) { s0 = animS1_; e0 = animE1_; animating_ = false; }
        setView(viewCi_, s0, e0);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_AppBack) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {  // Android back
        if (packCard_) {}
        else if (phoneSearch_) { phoneSearch_ = false; searchOpen_ = false; ImGui::SetWindowFocus(nullptr); }
        else if (phoneMenu_) phoneMenu_ = false;
        else if (legendOpen_) legendOpen_ = false;
        else if (pinned_.valid) { pinned_ = {}; sheetState_ = 0; }
    }
    const bool modal = packCard_ || phoneSearch_ || phoneMenu_;
    ImVec2 realMouse = io.MousePos;
    if (modal) io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
    // sheets cover the bottom of the region; the region's touch area stops at their top edge
    float sheetTop = D.y;
    if (legendOpen_) sheetTop = std::max(kPhoneTopH + 8, D.y - D.y * 0.62f);
    else if (pinned_.valid) sheetTop = D.y - (sheetState_ == 0 ? kSheetPeek : sheetState_ == 1 ? D.y * 0.5f : D.y - kPhoneTopH - 8);
    phonePinch();
    if (!modal && realMouse.y >= sheetTop && !phoneSearch_) io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);  // the sheet owns touches over it
    const float rTop = kPhoneTopH + kPhoneChipH;
    drawRegion(dl, ImVec2(4, rTop), ImVec2(D.x - 4, std::max(rTop + 80, sheetTop - 4)), fog);
    drawChips(dl, ImVec2(0, kPhoneTopH), ImVec2(D.x, rTop));
    drawPhoneTopBar(dl, D.x);
    io.MousePos = realMouse;
    if (phoneMenu_) io.MousePos = realMouse;
    if (!phoneSearch_ && !packCard_) {
        if (phoneMenu_) io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        if (legendOpen_) drawLegendSheet(D, fog);
        else drawCardSheet(dl, D);
        io.MousePos = realMouse;
    }
    if (phoneMenu_) drawPhoneMenu(D);
    if (phoneSearch_) {
        dl->AddRectFilled(ImVec2(0, 0), D, col(kBg));
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(D.x, kPhoneTopH), col(kPanel));
        ImGui::SetCursorScreenPos(ImVec2(2, 2));
        if (ImGui::InvisibleButton("##searchback", ImVec2(44, 44))) { phoneSearch_ = false; searchOpen_ = false; }
        dl->AddLine(ImVec2(16, 24), ImVec2(30, 24), col(kText), 2.f);
        dl->AddLine(ImVec2(16, 24), ImVec2(22, 18), col(kText), 2.f);
        dl->AddLine(ImVec2(16, 24), ImVec2(22, 30), col(kText), 2.f);
        if (phoneSearch_) drawSearch(D.x);
        if (phoneSearch_ && searchBuf_[0] == 0) {
            ImGui::PushFont(fonts().ui13);
            dl->AddText(nullptr, 0.f, ImVec2(16, 64), col(kMuted),
                        rsid_.present() ? "Type a gene, an rsID or a position like chr22:20,340,000." : "Type a gene or a position like chr22:20,340,000.",
                        nullptr, D.x - 32);
            ImGui::PopFont();
        }
    }
    drawPackCard(D);
    ImGui::End();
}

}  // namespace wisp
