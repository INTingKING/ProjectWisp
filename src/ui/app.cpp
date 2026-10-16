#include "ui/app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "ui/theme.h"

namespace wisp {

using namespace theme;

namespace {

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
void hatch(ImDrawList* dl, float x0, float y0, float x1, float y1) {
    if (x1 - x0 < 1.f) x1 = x0 + 1.f;
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    float h = y1 - y0;
    float start = std::floor((x0 - h - y0) / 6.f) * 6.f + y0;
    for (float x = start; x < x1 + 6.f; x += 6.f) dl->AddLine(ImVec2(x, y1), ImVec2(x + h, y0), col(kColNotIn, 0.7f), 1.f);
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
const char* catName(int c) {
    switch (c) {
        case kExon: return "Known function: exon";
        case kCcre: return "Known function: regulatory element";
        case kIntron: return "Intron";
        case kRepeat: return "Repeat";
        case kNothing: return "Nothing known";
        default: return "Not in GRCh38";
    }
}
const char* catBlurb(int c) {
    switch (c) {
        case kExon: return "Part of a gene that ends up in its RNA (and often in a protein).";
        case kCcre: return "ENCODE candidate cis-regulatory element: a switch that helps turn genes on or off.";
        case kIntron: return "Inside a gene but spliced out of the RNA. Mostly unknown function.";
        case kRepeat: return "A known repeat family (transposon remnant, satellite...). We know what it is, not what it does.";
        default: return "No gene, regulatory element or repeat annotation reaches here yet.";
    }
}
void sectionLabel(const char* t) {
    ImGui::PushFont(fonts().uiBold);
    ImGui::TextColored(vec(kMuted), "%s", t);
    ImGui::PopFont();
}

}  // namespace

bool App::init(const AppOptions& opt, std::string& err) {
    opt_ = opt;
    std::string tracks = opt.dataDir + "/tracks";
    if (!ov_.load(tracks + "/overview.wov", err)) return false;
    if (!store_.open(tracks, err)) return false;
    if (!genome_.open(opt.dataDir + "/hs1.fa.bgz", err)) return false;
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

bool App::idle() const { return store_.pending() == 0; }

void App::gather(int ci, int64_t s, int64_t e, Visible& v) {
    const std::string& chrom = ov_.chroms()[ci].name;
    std::set<std::tuple<int64_t, int64_t, std::string>> seenGenes;
    for (int64_t t = s / TrackStore::kTile; t <= (e - 1) / TrackStore::kTile; ++t) {
        for (int k = 0; k < 4; ++k) {
            TilePtr tile = store_.request(Track(k), chrom, t);
            if (!tile) { v.loading[k] = true; continue; }
            v.hold.push_back(tile);
            for (auto& g : tile->genes)
                if (g.e > s && g.s < e && seenGenes.insert({g.s, g.e, g.name}).second) v.genes.push_back(&g);
            for (auto& x : tile->exons) if (x.e > s && x.s < e) v.exons.push_back(&x);
            for (auto& x : tile->ccres) if (x.e > s && x.s < e) v.ccres.push_back(&x);
            for (auto& x : tile->repeats) if (x.e > s && x.s < e) v.repeats.push_back(&x);
            for (auto& x : tile->notin) if (x.e > s && x.s < e) v.notin.push_back(&x);
        }
    }
}

void App::frame(FogRenderer& fog) {
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

    drawTopBar(dl, D.x);
    float y = drawCoverage(dl, kTopBarH, D.x);
    float leftW = D.x - kMargin * 3 - kCardW;
    ImVec2 ovA(kMargin, y), ovB(kMargin + leftW, y + kOverviewH);
    ImVec2 rgA(kMargin, ovB.y + 12), rgB(kMargin + leftW, D.y - kMargin);
    ImVec2 cdA(D.x - kMargin - kCardW, y), cdB(D.x - kMargin, D.y - kMargin);
    drawOverview(dl, ovA, ovB, fog);
    drawRegion(dl, rgA, rgB, fog);
    drawCard(dl, cdA, cdB);
    ImGui::End();
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
    const float sx0 = A.x + 12 + gutter, sxMax = B.x - 14;
    double maxLen = 0;
    for (int ci : order_) maxLen = std::max(maxLen, double(ov_.chroms()[ci].length));
    std::vector<FogRect> fogRects;
    struct Hatch { float x0, y0, x1, y1; };
    std::vector<Hatch> hatches;
    ImGuiIO& io = ImGui::GetIO();
    const double bin = ov_.binSize();
    ImGui::PushFont(fonts().monoSmall);
    for (int r = 0; r < n; ++r) {
        int ci = order_[r];
        const auto& ch = ov_.chroms()[ci];
        float y = top + r * (stripH + gap), yb = y + stripH;
        float sw = float((sxMax - sx0) * double(ch.length) / maxLen);
        double bpPerPx = double(ch.length) / sw;
        std::string label = ch.name.substr(0, 3) == "chr" ? ch.name.substr(3) : ch.name;
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(sx0 - 6 - ts.x, y + (stripH - ts.y) * 0.5f), col(ci == viewCi_ ? kAccent : kMuted), label.c_str());
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
    const float gutter = 84;
    const float tx0 = A.x + 12 + gutter, tx1 = B.x - 14, W = tx1 - tx0;
    regionWidthPx_ = W;
    const double span = viewE_ - viewS_, bpPerPx = span / W, pxPerBp = W / span;
    auto X = [&](double pos) { return float(tx0 + (pos - viewS_) * pxPerBp); };

    // header: locus + go-to box
    std::string locus = ch.name + ":" + commas(int64_t(viewS_) + 1) + "-" + commas(int64_t(viewE_));
    ImGui::PushFont(fonts().uiBold);
    dl->AddText(ImVec2(A.x + 12, A.y + 10), col(kText), "Region");
    ImGui::PopFont();
    ImGui::PushFont(fonts().mono);
    dl->AddText(ImVec2(A.x + 74, A.y + 11), col(kAccent), locus.c_str());
    float lw = ImGui::CalcTextSize(locus.c_str()).x;
    ImGui::PopFont();
    dl->AddText(ImVec2(A.x + 74 + lw + 12, A.y + 10), col(kMuted), (spanStr(span) + "   wheel zoom | drag pan | click pin").c_str());
    ImGui::SetCursorScreenPos(ImVec2(B.x - 12 - 230, A.y + 6));
    ImGui::SetNextItemWidth(230);
    if (ImGui::InputTextWithHint("##goto", "go to chr22:30,000,000-30,050,000", gotoBuf_, sizeof gotoBuf_,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        int ci;
        double s, e;
        if (parseLocus(gotoBuf_, ci, s, e)) setView(ci, s, e);
    }

    // track heights (genes 48, cCRE 24, repeats 24); each track has a 14px header line for hints
    const float hdrH = 14, geneTrackH = 48, ccreH = 24, repH = 24, niH = 14;
    const float stackH = 26 + (10 + 8) + (hdrH + 20 + 6) + (hdrH + geneTrackH + 6) + (hdrH + ccreH + 6) + (hdrH + repH + 6) + niH;
    const float availTop = A.y + 40, availBot = B.y - 30;
    float y = availTop + std::max(0.f, (availBot - availTop - stackH) * 0.5f);
    auto hint = [&](float hy, const char* t) {
        ImGui::PushFont(fonts().monoSmall);
        dl->AddText(ImVec2(tx0, hy + 1), col(kMuted), t);
        ImGui::PopFont();
    };
    // ruler
    {
        double raw = span / 8.0, p10 = std::pow(10.0, std::floor(std::log10(raw)));
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
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(A.x + 12, ly + (lh - ts.y) * 0.5f), col(kMuted), t);
    };
    std::vector<FogRect> fogRects;
    struct Hatch { float x0, y0, x1, y1; };
    std::vector<Hatch> hatches;
    const int cols = std::max(1, int(W));
    const bool detailed = span <= double(kTrackMaxSpan);
    std::vector<uint8_t> cat(cols, kNothing);  // per-pixel category for the fog
    std::vector<uint8_t> catKnown(cols, 1);     // 0 while that pixel's data is still loading

    // lanes
    const float catY = y, catH = 10;
    y += catH + 8;
    const float seqHdrY = y;
    const float seqY = y + hdrH, seqH = 20;
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
    if (span <= 20000) {
        int64_t s = int64_t(viewS_), e = int64_t(std::ceil(viewE_));
        std::string seq = genome_.fetch(ch.name, s, e);
        auto baseCol = [](char b) -> unsigned {
            switch (std::toupper((unsigned char)b)) {
                case 'A': return 0x5CC48A;
                case 'C': return 0x6AA8FF;
                case 'G': return 0xF2B544;
                case 'T': return 0xF07178;
                default: return 0x8B93A1;
            }
        };
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
        for (auto& [row, g] : placed) {
            float ry = geneY + row * rowH;
            float gx0 = X(double(g->s)), gx1 = std::max(X(double(g->e)), gx0 + 1);
            float bandY = ry + 17;
            dl->AddRectFilled(ImVec2(std::max(gx0, tx0), bandY - 2), ImVec2(std::min(gx1, tx1), bandY + 2), col(kColIntron));
            // strand chevrons on the intron band
            if (gx1 - gx0 > 40) {
                float dir = g->strand == '-' ? -1.f : 1.f;
                for (float cx = std::max(gx0, tx0) + 20; cx < std::min(gx1, tx1) - 10; cx += 40)
                    dl->AddLine(ImVec2(cx - 3 * dir, bandY - 3), ImVec2(cx + 1 * dir, bandY), col(kMuted)),
                        dl->AddLine(ImVec2(cx + 1 * dir, bandY), ImVec2(cx - 3 * dir, bandY + 3), col(kMuted));
            }
            float lx = std::clamp(gx0, tx0, std::max(tx0, tx1 - 60));
            dl->AddText(ImVec2(lx, ry + 1), col(kText, 0.9f), g->name.c_str());
        }
        for (auto* x : vis.exons) {
            auto it = byName.find(x->gene);
            if (it == byName.end()) continue;
            for (auto& [row, g] : it->second) {
                if (x->s < g->s || x->e > g->e) continue;
                float ry = geneY + row * rowH;
                float x0 = std::max(tx0, X(double(x->s))), x1 = std::min(tx1, std::max(X(double(x->e)), X(double(x->s)) + 1.f));
                dl->AddRectFilled(ImVec2(x0, ry + 12), ImVec2(x1, ry + 22), col(kColKnown));
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
    laneLabel(repY, repH, "Repeats");
    laneLabel(niY, niH, "Not in GRCh38");
    if (detailed) {
        ImGui::PushFont(fonts().mono);
        for (auto* c : vis.ccres) {
            float x0 = std::max(tx0, X(double(c->s))), x1 = std::min(tx1, std::max(X(double(c->e)), x0 + 1));
            dl->AddRectFilled(ImVec2(x0, ccreY), ImVec2(x1, ccreY + ccreH), col(kColCcre));
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
            dl->AddRectFilled(ImVec2(x0, niY), ImVec2(x1, niY + niH), col(kColNotIn, 0.18f));
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
    const float tracksBottom = niY + niH;

    // fog over 'nothing known' columns, spanning all lanes
    for (int p = 0; p < cols;) {
        if (cat[p] != kNothing || !catKnown[p]) { ++p; continue; }
        int q = p;
        while (q < cols && cat[q] == kNothing && catKnown[q]) ++q;
        fogRects.push_back({tx0 + p - 2, tracksTop - 4, tx0 + q + 2, tracksBottom + 4, 0.55f, 24.f, 10.f});
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
    dl->AddText(ImVec2(A.x + 12, B.y - 24), col(kMuted, 0.8f), status);

    // interaction over the lanes
    ImGui::SetCursorScreenPos(ImVec2(tx0, tracksTop - 26));
    ImGui::InvisibleButton("##region", ImVec2(W, std::max(10.f, tracksBottom - tracksTop + 26)));
    if (ImGui::IsItemHovered()) {
        double pos = viewS_ + (io.MousePos.x - tx0) * bpPerPx;
        hover_ = {true, viewCi_, int64_t(std::clamp(pos, 0.0, double(ch.length) - 1)), false};
        if (io.MouseWheel != 0) {
            double f = std::pow(1.25, -io.MouseWheel);
            double ns = pos - (pos - viewS_) * f, ne = pos + (viewE_ - pos) * f;
            setView(viewCi_, ns, ne);
        }
        float x = io.MousePos.x;
        dl->AddLine(ImVec2(x, tracksTop - 6), ImVec2(x, tracksBottom + 4), col(kAccent, 0.6f));
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.f)) {
        double d = -io.MouseDelta.x * bpPerPx;
        setView(viewCi_, viewS_ + d, viewE_ + d);
    }
    if (ImGui::IsItemDeactivated() && io.MouseDragMaxDistanceSqr[0] < 9.f && hover_.valid) pinned_ = hover_;
    if (pinned_.valid && !pinned_.overview && pinned_.ci == viewCi_ && pinned_.pos >= viewS_ && pinned_.pos <= viewE_) {
        float x = X(double(pinned_.pos) + 0.5);
        dl->AddLine(ImVec2(x, tracksTop - 8), ImVec2(x, tracksBottom + 4), col(kAccent), 1.5f);
        dl->AddTriangleFilled(ImVec2(x - 5, tracksTop - 12), ImVec2(x + 5, tracksTop - 12), ImVec2(x, tracksTop - 5), col(kAccent));
    }
}

void App::drawCard(ImDrawList* dl, ImVec2 A, ImVec2 B) {
    dl->AddRectFilled(A, B, col(kPanel), 8);
    dl->AddRect(A, B, col(kBorder), 8);
    ImGui::SetCursorScreenPos(ImVec2(A.x + 16, A.y + 12));
    ImGui::BeginChild("##card", ImVec2(B.x - A.x - 32, B.y - A.y - 24), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::PushTextWrapPos(0.f);
    Probe p = hover_.valid ? hover_ : pinned_;
    ImGui::PushFont(fonts().title);
    ImGui::TextUnformatted("Lookup");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
    ImGui::TextColored(vec(kMuted), "%s", !p.valid ? "hover or click the map" : hover_.valid ? "under cursor" : "pinned");
    ImGui::Spacing();
    if (!p.valid) {
        ImGui::TextColored(vec(kMuted), "Point at any chromosome or region to see what is known there.");
        ImGui::EndChild();
        return;
    }
    const auto& ch = ov_.chroms()[p.ci];
    sectionLabel("POSITION");
    ImGui::PushFont(fonts().mono);
    ImGui::TextColored(vec(kAccent), "%s:%s", ch.name.c_str(), commas(p.pos + 1).c_str());
    ImGui::PopFont();
    ImGui::Spacing();

    auto chip = [&](unsigned c) {
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cp.x, cp.y + 3), ImVec2(cp.x + 12, cp.y + 15), col(c), 3);
        ImGui::Dummy(ImVec2(12, 16));
        ImGui::SameLine();
    };
    auto notInCallout = [&]() {
        ImGui::Spacing();
        ImVec2 cp = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->ChannelsSplit(2);
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos(ImVec2(cp.x + 10, cp.y + 8));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 20);
        ImGui::TextColored(vec(0xD9CCF5), "%s", kNotInMsg);
        ImGui::PopTextWrapPos();
        float h = ImGui::GetCursorScreenPos().y - cp.y + 4;
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(0);
        ImGui::GetWindowDrawList()->AddRectFilled(cp, ImVec2(cp.x + w, cp.y + h), col(kColNotIn, 0.28f), 6);
        ImGui::GetWindowDrawList()->AddRect(cp, ImVec2(cp.x + w, cp.y + h), col(kColNotIn), 6);
        ImGui::GetWindowDrawList()->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(cp.x, cp.y + h + 6));
        ImGui::Dummy(ImVec2(1, 1));
    };

    if (p.overview) {
        uint32_t b = uint32_t(p.pos / ov_.binSize());
        float f[kCatCount];
        for (int k = 0; k < kCatCount; ++k) f[k] = ov_.frac(p.ci, b, Cat(k));
        int best = kNothing;
        for (int k = 0; k < kNotIn; ++k)
            if (f[k] > f[best]) best = k;
        sectionLabel("MOSTLY");
        chip(catColor(best));
        ImGui::PushFont(fonts().uiBold);
        ImGui::TextUnformatted(catName(best));
        ImGui::PopFont();
        ImGui::TextColored(vec(kMuted), "%s", catBlurb(best));
        ImGui::Spacing();
        sectionLabel("THIS 100 KB BIN");
        ImGui::TextColored(vec(kMuted), "%s:%s-%s", ch.name.c_str(), commas(int64_t(b) * ov_.binSize() + 1).c_str(),
                           commas(std::min<int64_t>(int64_t(b + 1) * ov_.binSize(), int64_t(ch.length))).c_str());
        const char* names[kCatCount] = {"Exons", "cCREs", "Introns", "Repeats", "Nothing known", "Not in GRCh38"};
        for (int k = 0; k < kCatCount; ++k) {
            ImVec2 cp = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            auto* wdl = ImGui::GetWindowDrawList();
            ImGui::Text("%s", names[k]);
            ImGui::SameLine(w - 52);
            ImGui::TextColored(vec(kMuted), "%5.1f%%", f[k] * 100);
            float bx = cp.x + 118, bw = w - 118 - 60;
            wdl->AddRectFilled(ImVec2(bx, cp.y + 6), ImVec2(bx + bw, cp.y + 12), col(kHover), 3);
            if (k == kNotIn) {
                wdl->AddRectFilled(ImVec2(bx, cp.y + 6), ImVec2(bx + bw * f[k], cp.y + 12), col(kColNotIn, 0.4f), 3);
                hatch(wdl, bx, cp.y + 6, bx + bw * f[k], cp.y + 12);
            } else {
                wdl->AddRectFilled(ImVec2(bx, cp.y + 6), ImVec2(bx + bw * f[k], cp.y + 12), col(catColor(k)), 3);
            }
        }
        if (f[kNotIn] > 0.5f) notInCallout();
        ImGui::Spacing();
        ImGui::TextColored(vec(kMuted), "Click to open the whole chromosome, drag to zoom.");
    } else {
        int64_t tile = p.pos / TrackStore::kTile;
        Visible v;
        gather(p.ci, std::max<int64_t>(0, p.pos), p.pos + 1, v);
        (void)tile;
        bool loading = v.loading[0] || v.loading[1] || v.loading[2] || v.loading[3];
        int c = kNothing;
        bool inExon = false;
        for (auto* x : v.exons) inExon |= (x->s <= p.pos && p.pos < x->e);
        if (inExon) c = kExon;
        else if (!v.ccres.empty()) c = kCcre;
        else if (!v.genes.empty()) c = kIntron;
        else if (!v.repeats.empty()) c = kRepeat;
        sectionLabel("CATEGORY");
        if (loading) {
            ImGui::TextColored(vec(kMuted), "loading annotations...");
        } else {
            chip(catColor(c));
            ImGui::PushFont(fonts().uiBold);
            ImGui::TextUnformatted(catName(c));
            ImGui::PopFont();
            ImGui::TextColored(vec(kMuted), "%s", catBlurb(c));
        }
        if (!v.notin.empty()) notInCallout();
        std::string base = genome_.fetch(ch.name, p.pos, p.pos + 1);
        ImGui::Spacing();
        sectionLabel("BASE");
        ImGui::PushFont(fonts().mono);
        ImGui::Text("%s%s", base.empty() ? "?" : std::string(1, char(std::toupper((unsigned char)base[0]))).c_str(),
                    !base.empty() && std::islower((unsigned char)base[0]) ? "   (soft-masked: repeat)" : "");
        ImGui::PopFont();
        ImGui::Spacing();
        sectionLabel("FEATURES HERE");
        int shown = 0;
        for (auto* g : v.genes) {
            bool ex = false;
            for (auto* x : v.exons) ex |= (x->gene == g->name && x->s <= p.pos && p.pos < x->e);
            chip(ex ? kColKnown : kColIntron);
            ImGui::PushFont(fonts().uiBold);
            ImGui::TextUnformatted(g->name.c_str());
            ImGui::PopFont();
            ImGui::TextColored(vec(kMuted), "gene, %s, %c strand, %s; %s here", g->biotype.empty() ? "unknown type" : g->biotype.c_str(),
                               g->strand, spanStr(double(g->e - g->s)).c_str(), ex ? "exon" : "intron");
            ++shown;
        }
        for (auto* c2 : v.ccres) {
            chip(kColCcre);
            ImGui::Text("cCRE %s", c2->id.c_str());
            ImGui::TextColored(vec(kMuted), "regulatory element, class %s, %s", c2->cls.c_str(), spanStr(double(c2->e - c2->s)).c_str());
            ++shown;
        }
        for (auto* r : v.repeats) {
            chip(kColRepeat);
            ImGui::Text("Repeat %s", r->name.c_str());
            ImGui::TextColored(vec(kMuted), "%s / %s, %s", r->cls.c_str(), r->family.c_str(), spanStr(double(r->e - r->s)).c_str());
            ++shown;
        }
        if (!shown && !loading) ImGui::TextColored(vec(kMuted), "No annotated features at this base.");
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

}  // namespace wisp
