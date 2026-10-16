# ProjectWisp

A wisp to guide you through the fog of the human genome.

ProjectWisp is a local genome explorer built on the gap-free **T2T-CHM13 v2.0** reference (UCSC `hs1`, NCBI `GCF_009914755.1`). Pick a gene or position and it tells you, in plain language, what is there: gene or exon, repeat family, regulatory element, known variation, and what is still unknown.

The genome overview is a map. Regions are split three ways:
- **Function known** (genes, regulatory elements): clear and coloured.
- **Identified, function unknown** (most repeats and satellites): thin mist.
- **Nothing known**: fog.

The headline number, "X% has a known function", is computed from the loaded layers, never typed in. When a layer adds knowledge, the fog lifts.

Long-term direction: map non-coding DNA to biological mechanisms, and eventually model a human body with genome variables covering human diversity (pangenome). That is far beyond current science; the explorer is step one.

## Status
Milestone 1 (in progress): native local explorer with CHM13 sequence, genes, repeats and lifted ENCODE regulatory elements (variants not shown yet).

## Native explorer (milestone 1)
A native C++20 app (SDL3 + Dear ImGui + Vulkan) that reads only the visible region of each track through htslib.

Layout: `src/data` (overview summary, tabix tile cache, faidx sequence), `src/render` (Vulkan context via vk-bootstrap + VMA, fog pipeline, PNG output), `src/ui` (theme, app layout), `src/shaders` (fog GLSL, compiled to SPIR-V at build time). Fonts: Inter and JetBrains Mono under `assets/fonts` (SIL OFL, licence files alongside).

### 1. Prepare the tracks (once)
Needs `bgzip`/`tabix` (htslib) and Python 3 with numpy. Reads the files in `data/` and writes `data/tracks/`:
`genes.gff.gz` (gene + exon features), `ccre.bed.gz`, `repeats.bed.gz`, `notgrch38.bed.gz` (all bgzipped + `.tbi`) and `overview.wov`, a ~370 KB per-100 kb-bin summary (exon, cCRE outside exons, intron, repeat, nothing, not-in-GRCh38). Nothing in `data/` is modified or deleted.
```sh
scripts/prepare_tracks.sh            # ~2-3 min, ~3 GB RAM peak for the summary
```
The headline "X% has a known function" and the coverage bar are computed from `overview.wov` at runtime.

### 2. Build
Dependencies fetched by CMake (FetchContent): SDL3, Dear ImGui, vk-bootstrap, VulkanMemoryAllocator, stb. Provided by the system: a Vulkan loader + headers, `glslangValidator`, htslib (found via pkg-config).

**Linux (Debian/Ubuntu)**
```sh
sudo apt-get install build-essential cmake pkg-config libhts-dev tabix libvulkan-dev glslang-tools \
     mesa-vulkan-drivers libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```
No GPU? `mesa-vulkan-drivers` provides lavapipe (CPU Vulkan); it is enough for both the window and screenshots.

**macOS** (MoltenVK): install the LunarG Vulkan SDK (includes MoltenVK, loader, glslangValidator) and `brew install htslib cmake pkg-config`, then `source ~/VulkanSDK/<ver>/setup-env.sh` and build as above. vk-bootstrap enables `VK_KHR_portability_enumeration` and `VK_KHR_portability_subset` automatically when the driver exposes them, which MoltenVK requires.

**Windows**: install the LunarG Vulkan SDK, then htslib from vcpkg (`vcpkg install htslib:x64-windows pkgconf`, configure with `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`) or MSYS2 UCRT64 (`pacman -S mingw-w64-ucrt-x86_64-{htslib,cmake,gcc,pkgconf,vulkan-devel,glslang}`) and build with the same cmake commands. Not yet tested on Windows or macOS.

### 3. Run
```sh
./build/wisp                                   # window; run from the repo root (or --data DIR)
./build/wisp --region chr22:20,310,000-20,360,000 --pin chr22:20,339,900
./build/wisp --reduced-motion                  # fog does not drift
# offscreen screenshot, no window or display needed (renders into a Vulkan image, writes PNG):
./build/wisp --shot out.png --size 1600x1000 --region chr22:20,339,821-20,339,940 --time 2
```
Variants: the first `*.vcf.gz` with a `.tbi` in `data/` (here the 1KGP CHM13 chr22 sites file from `fetch_data.sh`, override with `--variants FILE`) is read directly through tabix in the same 1 Mb blocks, drawn as 1 px ticks or per-pixel density bars (brighter = AF >= 1%).

Structural variants: `scripts/fetch_sv.sh` (also called from `fetch_data.sh`) downloads the HPRC v1.1 Minigraph-Cactus CHM13 VCF (3.0 GB), keeps sites only, splits multi-allelic records, keeps ins/del/complex with abs(LEN) >= 50 plus INV-flagged records, writes `data/tracks/sv.hprc.vcf.gz` (~91 MB + .tbi) and deletes the raw file. Needs bcftools and ~8 GB free while it runs.

CpG methylation: `fetch_data.sh` downloads the CHM13 nanopore CpG bigWig (177 MB) to `data/tracks/`; libBigWig (FetchContent, no curl) reads it per 1 Mb tile through the same loader/cache. It is drawn as a 4 px strip at the bottom of the regulatory track (#2A2F3A low -> #C9A6FF high, per-pixel mean).

Interaction: click a chromosome in the overview (or drag across it) to open it; mouse wheel zooms, drag pans, click pins the lookup card; the search box in the top bar (Ctrl+K or /) finds genes by symbol or alias (extra Liftoff copies listed under their main copy) and accepts a locus such as chr22:20,340,000. Above 3 Mb the region view draws densities from the summary; below 3 Mb features load from tabix in 1 Mb tiles (LRU cache, ~100 MB cap); below 20 kb the sequence lane appears, with A/C/G/T letters once a base is at least 8 px wide.

## Data
Genome and annotation files are **not** in this repo (10 to 20 GB). A fetch script will download them:
- Sequence: UCSC `hs1.fa.gz`, stored bgzipped with `samtools faidx`.
- liftOver chains `hg38ToHs1` / `hs1ToHg38` from UCSC.

### Data credits
`scripts/fetch_data.sh` downloads everything below (it calls `scripts/fetch_sv.sh` for the SV step). Each file keeps its own licence.
- **T2T-CHM13 v2.0 sequence** (`hs1.fa.gz`) and liftOver chains, via UCSC: https://hgdownload.soe.ucsc.edu/goldenPath/hs1/ . Nurk S. et al., *The complete sequence of a human genome*, Science 376:44-53 (2022). The UCSC liftOver chain files are free for non-commercial use only; commercial use needs a licence from UCSC.
- **CHM13 annotations** (RefSeq Liftoff v5.3 genes, RepeatMasker 4.1.2p1, `chm13v2-unique_to_hg38.bed`) from the T2T consortium bucket: https://s3-us-west-2.amazonaws.com/human-pangenomics/T2T/CHM13/ (open data, CC0 / public per the T2T consortium).
- **ENCODE cCREs** (`encodeCcreCombined.bb`, hg38, lifted to hs1): https://hgdownload.soe.ucsc.edu/gbdb/hg38/encode3/ccre/ . ENCODE Project Consortium, Nature 583:699-710 (2020). ENCODE data are free to use.
- **1000 Genomes on CHM13** (chr22 allele-frequency VCF): https://s3-us-west-2.amazonaws.com/human-pangenomics/T2T/CHM13/assemblies/variants/1000_Genomes_Project/chm13v2.0/ . Aganezov S. et al., Science 376:eabl3533 (2022).
- **HPRC structural variants** (Minigraph-Cactus v1.1, CHM13-based, `hprc-v1.1-mc-chm13.vcfbub.a100k.wave.vcf.gz`, filtered locally by `fetch_sv.sh` to sites with |LEN| >= 50 plus INV-flagged records): https://s3-us-west-2.amazonaws.com/human-pangenomics/pangenomes/freeze/freeze1/minigraph-cactus/hprc-v1.1-mc-chm13/ . Liao W.-W. et al., *A draft human pangenome reference*, Nature 617:312-324 (2023); Hickey G. et al., Nature Biotechnology 42:663-673 (2024). HPRC data are released openly (HPRC data use: open access, cite the papers).
- **CHM13 CpG methylation** (nanopore, guppy 3.6.0 + nanopolish 0.13.2, bigWig): https://s3-us-west-2.amazonaws.com/human-pangenomics/T2T/CHM13/assemblies/annotation/regulation/chm13v2.0_CHM13_CpG_ont_guppy3.6.0_nanopolish0.13.2.bw . Gershman A. et al., *Epigenetic patterns in a complete human genome*, Science 376:eabj5089 (2022). T2T consortium open data.

## Licence and usage
Everything made for this project (code, logo, graphics, docs) is MIT licensed (see `LICENSE`).

Files taken from elsewhere are not: they keep their original licences. Genome and annotation files are downloaded at runtime from their sources and never redistributed here, and each keeps its own terms. The UCSC liftOver chain files in particular are free only for non-commercial use. If you build something commercial on ProjectWisp, check every data source's terms first.
