#pragma once
// ProjectWisp UI: top bar + coverage bar, genome overview, region view with tracks, lookup card.
#include <imgui.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <string>
#include <vector>

#include "data/geneindex.h"
#include "data/rsid.h"
#include "data/genome.h"
#include "data/overview.h"
#include "data/tracks.h"
#include "render/fog.h"

namespace wisp {

struct AppOptions {
    std::string dataDir = "data";
    std::string assetsDir = "assets";  // set from the executable location (data_release.conf lives here)
    std::string region;  // "chr22:30,000,000-30,050,000" or "chr22"
    std::string pin;     // "chr22:12345" pins the lookup card
    std::string variants;  // bgzipped+tabixed VCF; default: first *.vcf.gz with .tbi in dataDir
    bool reducedMotion = false;
    bool expandSv = false;  // --expand sv
    bool expandCard = false;  // --expand-card
    std::string rsidPack;  // --rsid-pack PATH (default data/packs/rsid/rsid.idx)
    bool packCard = false;  // --pack-card: open the optional-download card
    bool packDownload = false;  // --pack-download: start the background pack download at launch
    std::string searchGroup;  // --search-group SYMBOL
    bool searchMore = false;  // --search-more: expanded "+N more" list
    std::string search;  // --search QUERY: open the search box with this text
    std::string gotoQuery;  // --goto QUERY: jump as if Enter was pressed
    std::string settingsPath;  // settings.ini; empty = no persistence (shot/script mode unless --settings)
    bool legend = false;       // --legend: open the legend panel (overrides settings)
    bool intro = false;        // --intro: start the first-run intro (overrides settings)
};

struct Probe {
    bool valid = false;
    int ci = -1;
    int64_t pos = 0;
    bool overview = false;  // came from the overview strips (bin-level)
};

class App {
public:
    ~App();
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
    void drawPackCard(ImVec2 display);
    bool runQuery(const std::string& q);  // jump to the best match; false if nothing matched
    void jumpTo(int ci, double s, double e);
    float drawCoverage(ImDrawList* dl, float y, float w);
    void drawOverview(ImDrawList* dl, ImVec2 a, ImVec2 b, FogRenderer& fog);
    void drawRegion(ImDrawList* dl, ImVec2 a, ImVec2 b, FogRenderer& fog);
    void drawCard(ImDrawList* dl, ImVec2 a, ImVec2 b);
    void drawTopButtons(float w);
    void drawLegend(ImVec2 a, ImVec2 b, FogRenderer& fog);
    void drawIntro(ImVec2 display);
    void startIntro();
    void endIntro();
    void loadSettings();
    void saveSettings() const;
    // legend panel + settings
    bool legendOpen_ = false;
    bool introDone_ = false;   // settings flag: first-run intro finished or skipped
    bool settingsOpen_ = false;
    // first-run intro: -1 closed, else step 0..2
    int introStep_ = -1;
    double introT0_ = -10;     // start of the current step's fade-in
    bool introPending_ = false;  // pack download finished: start the intro once its card closes
    int lastDlState_ = 0;
    float introCardH_ = 160.f;  // measured card height (placement above a target)
    ImVec2 ovA_{}, ovB_{}, rgA_{}, rgB_{}, searchA_{}, searchB_{};  // intro targets (last frame's layout)
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
    RsidIndex rsid_;
    RsidHit lastRs_{};          // last rsID jumped to (card shows its dbSNP line at that position)
    // background pack download: prebuilt rsid.idx from the GitHub data release, SHA-256 checked
    std::thread dlThread_;
    std::atomic<int> dlState_{0};  // 0 idle, 1 running, 2 done, 3 failed
    std::atomic<long long> dlDoneBytes_{0}, dlCurBytes_{0}, dlTotal_{0};  // finished files, current file, whole pack
    double dlT0_ = 0;  // virtual-clock start of the download (script mode progress)
    std::atomic<int> dlPhase_{0};  // 0 download, 1 verify
    struct PackFile { std::string pack, asset, path; long long bytes = 0; std::string sha; };
    std::vector<PackFile> packs_;  // assets/data_packs.tsv: what each downloadable pack contains
    std::string dlPack_;           // pack being (or last) downloaded
    void loadPacks();
    std::string packDest(const PackFile& f) const;
    long long packBytes(const std::string& pack) const;
    bool packInstalled(const std::string& pack) const;
    void startDownload(const std::string& pack);
    std::mutex dlMu_;
    std::string dlMsg_;
    bool manualOpen_ = false;
    void startPackDownload();
    std::string rsidSizeStr() const;
    bool packCard_ = false;
    char searchBuf_[96] = {};
    bool searchOpen_ = false, searchFocus_ = false;
    int searchSel_ = 0, searchScroll_ = 0;
    bool searchMore_ = false;  // "+N more" expanded into the scrollable list
    std::string openGroup_;  // extra-copy group opened via "+N copies" (sticky header in the expanded list)
    int scrollToGroup_ = 0;  // >0: next frame scroll so the group main row is (n-1) rows above the top
    ImVec2 dropA_{-1, -1}, dropB_{-1, -1};  // dropdown rect (blocks region hover/click under it)
    double clock_ = 0;
    double virtualClock_ = -1;  // app time (s), advanced by io.DeltaTime
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
    bool tilesPending() const { return store_.pending() > 0; }
    void setVirtualClock(double t) { virtualClock_ = t; }  // --script: app time comes from the script, not frame deltas
private:
    // 5th/95th percentile of per-bin share (known = exon+cCRE, nothing), for overview contrast stretch
    float knownP5_ = 0, knownP95_ = 1, nothingP5_ = 0, nothingP95_ = 1;
    float stretchAlpha(float share, float p5, float p95) const;
};

}  // namespace wisp
