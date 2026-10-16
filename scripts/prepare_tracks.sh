#!/usr/bin/env bash
# Build region-indexed tracks (bgzip + tabix) and the overview summary for the native explorer.
# Usage: scripts/prepare_tracks.sh [data_dir]   (default: data/). Writes into <data_dir>/tracks/. Deletes nothing.
set -euo pipefail
D="${1:-$(dirname "$0")/../data}"; D="$(cd "$D" && pwd)"; T="$D/tracks"; mkdir -p "$T"
export LC_ALL=C
need(){ command -v "$1" >/dev/null || { echo "missing $1 (apt install tabix)"; exit 1; }; }
need bgzip; need tabix
SORT="sort -S 1G -k1,1 -k2,2n"

echo "[1/5] genes+exons (GFF, attributes trimmed to gene name/biotype)"
zcat "$D/chm13v2.0_RefSeq_Liftoff_v5.3.gff.gz" | awk -F'\t' 'BEGIN{OFS="\t"}
  !/^#/ && ($3=="gene"||$3=="exon") {
    n="";b=""; k=split($9,a,";");
    for(i=1;i<=k;i++){ if(a[i]~/^gene=/)n=substr(a[i],6); else if(a[i]~/^gene_biotype=/)b=substr(a[i],14) }
    if(n=="") for(i=1;i<=k;i++) if(a[i]~/^gene_name=/) n=substr(a[i],11);
    attr="gene=" n; if(b!="") attr=attr ";biotype=" b;
    print $1,"Liftoff",$3,$4,$5,".",$7,".",attr }' \
  | sort -S 1G -k1,1 -k4,4n -k5,5n -u | bgzip -@4 > "$T/genes.gff.gz"
tabix -f -p gff "$T/genes.gff.gz"

echo "[2/5] cCREs"
grep -v '^#' "$D/ccre.hs1.bed" | $SORT | bgzip -@4 > "$T/ccre.bed.gz"; tabix -f -p bed "$T/ccre.bed.gz"

echo "[3/5] repeats (chrom start end name strand class family)"
awk -F'\t' 'BEGIN{OFS="\t"} !/^#/{print $1,$2,$3,$4,$6,$7,$8}' "$D/chm13v2.0_RepeatMasker_4.1.2p1.2022Apr14.bed" \
  | $SORT | bgzip -@4 > "$T/repeats.bed.gz"; tabix -f -p bed "$T/repeats.bed.gz"

echo "[4/5] not in GRCh38"
cut -f1-3 "$D/chm13v2-unique_to_hg38.bed" | grep -v '^#' | $SORT | bgzip > "$T/notgrch38.bed.gz"; tabix -f -p bed "$T/notgrch38.bed.gz"

echo "[5/5] overview summary"
python3 "$(dirname "$0")/make_overview.py" "$D" "$T/overview.wov"
ls -la "$T"
