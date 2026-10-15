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
Milestone 1 (in progress): local explorer with CHM13 sequence, genes, repeats, lifted ENCODE regulatory elements and variants.

## Data
Genome and annotation files are **not** in this repo (10 to 20 GB). A fetch script will download them:
- Sequence: UCSC `hs1.fa.gz`, stored bgzipped with `samtools faidx`.
- liftOver chains `hg38ToHs1` / `hs1ToHg38` from UCSC.

## Licence and usage
ProjectWisp is open source and **strictly non-commercial**. Some data, including the UCSC liftOver chain files, is free only for non-commercial use; it is downloaded at runtime and never redistributed here. Projects built on ProjectWisp may commercialise their own work only if every data source's terms allow it.
