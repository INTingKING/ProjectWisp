#pragma once
// ProjectWisp UI: top bar + coverage bar, genome overview, region view with tracks, lookup card.
#include <imgui.h>

#include <memory>
#include <string>
#include <vector>

#include "data/genome.h"
#include "data/overview.h"
#include "data/tracks.h"
#include "render/fog.h"

namespace wisp {

struct AppOptions {
    std::string dataDir = "data";
    std::string region;  // "chr22:30,000,000-30,050,000" or "chr22"
    std::string pin;     // "chr22:30012345" pins the lookup card
    bool reducedMotion = false;
};

struct Probe {
    bool valid = false;
    int ci = -1;
    int64_t pos = 0;
    bool overview = false;  // came from the overview strips (bin-level)
};

class App {
public:
    bool init(const AppOptions& opt, std::string& err);
    void frame(FogRenderer& fog);
    bool idle() const;
    bool reducedMotion() const { return opt_.reducedMotion; }

private:
    struct Visible {  // features merged from the tiles covering the view
        std::vector<const Gene*> genes;
        std::vector<const Exon*> exons;
        std::vector<const Ccre*> ccres;
        std::vector<const Repeat*> repeats;
        std::vector<const Interval*> notin;
        bool loading[4] = {};
        std::vector<TilePtr> hold;
    };
    void gather(int ci, int64_t s, int64_t e, Visible& v);
    void drawTopBar(ImDrawList* dl, float w);
    float drawCoverage(ImDrawList* dl, float y, float w);
    void drawOverview(ImDrawList* dl, ImVec2 a, ImVec2 b, FogRenderer& fog);
    void drawRegion(ImDrawList* dl, ImVec2 a, ImVec2 b, FogRenderer& fog);
    void drawCard(ImDrawList* dl, ImVec2 a, ImVec2 b);
    void setView(int ci, double s, double e);
    bool parseLocus(const std::string& txt, int& ci, double& s, double& e) const;

    AppOptions opt_;
    Overview ov_;
    TrackStore store_;
    Genome genome_;
    std::vector<int> order_;  // display order of chromosomes
    int viewCi_ = 0;
    double viewS_ = 0, viewE_ = 0;
    float regionWidthPx_ = 1000.f;
    Probe hover_, pinned_;
    // overview drag-select
    int dragCi_ = -1;
    float dragX0_ = 0;
    char gotoBuf_[96] = {};
    // 5th/95th percentile of per-bin share (known = exon+cCRE, nothing), for overview contrast stretch
    float knownP5_ = 0, knownP95_ = 1, nothingP5_ = 0, nothingP95_ = 1;
    float stretchAlpha(float share, float p5, float p95) const;
};

}  // namespace wisp
