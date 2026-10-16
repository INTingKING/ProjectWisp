#!/usr/bin/env bash
# Download ProjectWisp's genome + annotation data into data/ (not committed).
# Every file keeps its original licence; UCSC chain files are non-commercial only.
set -euo pipefail
D="${1:-data}"; mkdir -p "$D/tools"; cd "$D"
T2T=https://s3-us-west-2.amazonaws.com/human-pangenomics/T2T/CHM13/assemblies
UCSC=https://hgdownload.soe.ucsc.edu
get() { [ -s "$(basename "$1")" ] || curl -fL --retry 3 -O "$1"; }

OS=$(uname -s); ARCH=$(uname -m)
case "$OS" in Linux) KU=linux.x86_64;; Darwin) [ "$ARCH" = arm64 ] && KU=macOSX.arm64 || KU=macOSX.x86_64;; *) echo "use WSL on Windows"; exit 1;; esac
for t in bigBedToBed liftOver; do [ -x tools/$t ] || { curl -fL -o tools/$t $UCSC/admin/exe/$KU/$t; chmod +x tools/$t; }; done

# sequence
get $UCSC/goldenPath/hs1/bigZips/hs1.fa.gz
[ -s hs1.fa.bgz ] || { gzip -dc hs1.fa.gz | bgzip -@4 > hs1.fa.bgz; samtools faidx hs1.fa.bgz; }
# chains
get $UCSC/goldenPath/hs1/liftOver/hs1ToHg38.over.chain.gz
get $UCSC/goldenPath/hg38/liftOver/hg38ToHs1.over.chain.gz
# CHM13 layers
get $T2T/annotation/chm13v2.0_RefSeq_Liftoff_v5.3.gff.gz
get $T2T/annotation/chm13v2.0_RepeatMasker_4.1.2p1.2022Apr14.bed
get $T2T/chain/v1_nflo/chm13v2-unique_to_hg38.bed
V=$T2T/variants/1000_Genomes_Project/chm13v2.0/unrelated_samples_2504/allele_freq/1KGP.CHM13v2.0.chr22.recalibrated.snp_indel.pass.withafinfo.vcf.gz
get $V; get $V.tbi
# ENCODE cCREs (hg38) -> CHM13
get $UCSC/gbdb/hg38/encode3/ccre/encodeCcreCombined.bb
if [ ! -s ccre.hs1.bed ]; then
  tools/bigBedToBed encodeCcreCombined.bb ccre.hg38.bed
  cut -f1-9,11 ccre.hg38.bed > ccre.hg38.min.bed   # bed9 + class (PLS/pELS/dELS/CA-CTCF/...)
  tools/liftOver -bedPlus=9 -tab ccre.hg38.min.bed hg38ToHs1.over.chain.gz ccre.hs1.unsorted.bed ccre.unmapped.bed
  sort -k1,1 -k2,2n ccre.hs1.unsorted.bed > ccre.hs1.bed && rm ccre.hs1.unsorted.bed
fi
echo "done: $(pwd)"
