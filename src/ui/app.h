#pragma once
// ProjectWisp UI: top bar + coverage bar, genome overview, region view with tracks, lookup card.
#include <imgui.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "data/geneindex.h"
#include "data/genome.h"
#include "data/overview.h"
#include "data/tracks.h"
#include "render/fog.h"

namespace wisp {

struct AppOptions {
    std::string dataDir = "data";
    std::string region;  // "chr22:30,000,000-30,050,000" or "chr22"
    std::string pin;     // "chr22:12345" pins the lookup card
    std::string variants;  // bgzipped+tabixed VCF; default: first *.vcf.gz with .tbi in dataDir
    bool reducedMotion = false;
    bool expandSv = false;  // --expand sv
    bool expandCard = false;  // --expand-card
    bool searchMore = false;  // --search-more: expanded "+N more" list
    std::string search;  // --search QUERY: open the search box with this text
    std::string gotoQuery;  // --goto QUERY: jump as if Enter was pressed
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
        std::vector<const Variant*> variants;
        std::vector<const Sv*> svs;
        std::vector<const SvGroup*> svGroups;
        std::vector<const MethylSite*> methyl;
        bool loading[kTrackCount] = {};
        std::vector<TilePtr> hold;
    };
    void gather(int ci, int64_t s, int64_t e, Visible& v);
    void drawTopBar(ImDrawList* dl, float w);
    void drawSearch(float w);
    bool runQuery(const std::string& q);  // jump to the best match; false if nothing matched
    void jumpTo(int ci, double s, double e);
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
    // gene search
    GeneIndex genes_;
    char searchBuf_[96] = {};
    bool searchOpen_ = false, searchFocus_ = false;
    int searchSel_ = 0, searchScroll_ = 0;
    bool searchMore_ = false;  // "+N more" expanded into the scrollable list
    std::set<std::string> expandedGroups_;  // extra-copy groups opened via "+N copies"
    ImVec2 dropA_{-1, -1}, dropB_{-1, -1};  // dropdown rect (blocks region hover/click under it)
    double clock_ = 0;  // app time (s), advanced by io.DeltaTime
    bool animating_ = false;
    double animT0_ = 0, animS0_ = 0, animE0_ = 0, animS1_ = 0, animE1_ = 0;
    std::string hlName_, hlChrom_;
    int64_t hlS_ = -1;
    double hlT0_ = -10;
    bool svExpanded_ = false;
    bool cardSvExpanded_ = false;  // allele list in the lookup card (session only)  // per session only, never saved
    int svRowsLast_ = 1;
    double svMs_ = 0;  // last SV-track draw time (shot mode reports it)
public:
    double svMs() const { return svMs_; }
private:
    // 5th/95th percentile of per-bin share (known = exon+cCRE, nothing), for overview contrast stretch
    float knownP5_ = 0, knownP95_ = 1, nothingP5_ = 0, nothingP95_ = 1;
    float stretchAlpha(float share, float p5, float p95) const;
};

}  // namespace wisp
