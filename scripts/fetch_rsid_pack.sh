#!/usr/bin/env bash
# OPTIONAL rsID search pack (not part of the base bundle).
# Streams UCSC's dbSNP 155 on T2T-CHM13 v2.0 (~25 GB, never stored on disk) and keeps dbSNP's COMMON=1 variants
# (minor allele >= 1% in at least one 1000 Genomes population, 2+ founders): ID, CHROM, POS, REF, ALT.
# ALTs are trimmed with dbSNP's INFO/FREQ 1000Genomes entry (REF first, then each ALT): only ALTs with a
# 1000 Genomes frequency above 0 are kept; the number of dropped (rare) ALTs is stored. Records without a
# 1000Genomes entry keep all ALTs.
# Writes data/packs/rsid/rsid.idx (sorted binary, binary-searched from disk by the app) and rsid.tsv.gz (text copy).
# Prints "PROGRESS <bytes> <total>" and "PHASE <name>" lines on stderr for the app's download card.
set -euo pipefail
D="${1:-$(dirname "$0")/../data}"; D="$(cd "$D" && pwd)"; P="$D/packs/rsid"; mkdir -p "$P"
URL=https://hgdownload.soe.ucsc.edu/gbdb/hs1/dbSNP155/chm13v2.0_dbSNPv155.vcf.gz
for t in bgzip bcftools curl dd; do command -v $t >/dev/null || { echo "$t missing (apt install tabix bcftools)"; exit 1; }; done
[ -s "$P/rsid.idx" ] && { echo "already built: $P/rsid.idx"; exit 0; }
export LC_ALL=C
TOTAL=$(curl -fsSIL "$URL" | awk 'tolower($1)=="content-length:"{v=$2} END{gsub("\r","",v); print v+0}')
echo "PHASE download" >&2
curl -fsSL --retry 3 "$URL" \
  | dd bs=4M iflag=fullblock status=progress 2> >(tr '\r' '\n' | awk -v T="$TOTAL" '/bytes/{print "PROGRESS", $1, T; fflush()}' >&2) \
  | bgzip -d -@4 | grep -E '^#|COMMON' \
  | bcftools view -i 'INFO/COMMON=1' -Ou - \
  | bcftools query -f '%ID\t%CHROM\t%POS\t%REF\t%ALT\t%INFO/FREQ\n' \
  | awk -F'\t' 'BEGIN{OFS="\t"} substr($1,1,2)=="rs" {
      n = split($5, alt, ","); i = index($6, "1000Genomes:"); keep = $5; drop = 0
      if (i) {
        f = substr($6, i + 12); j = index(f, "|"); if (j) f = substr(f, 1, j - 1)
        split(f, q, ","); keep = ""
        for (a = 1; a <= n; a++) { v = q[a + 1]; if (v != "." && v != "" && v + 0 > 0) keep = keep (keep == "" ? "" : ",") alt[a]; else drop++ }
        if (keep == "") { keep = $5; drop = 0 }   # no ALT seen in 1000 Genomes: keep them all rather than show none
      }
      print substr($1, 3), $2, $3, $4, keep, drop }' \
  | { cat > "$P/rsid.unsorted.tsv"; }
echo "PHASE sort" >&2
sort -S 1G -T "$P" -n -k1,1 -u "$P/rsid.unsorted.tsv" | gzip -6 > "$P/rsid.tsv.gz.part"
rm -f "$P/rsid.unsorted.tsv"; mv "$P/rsid.tsv.gz.part" "$P/rsid.tsv.gz"
echo "PHASE index" >&2
python3 "$(dirname "$0")/build_rsid_index.py" "$P/rsid.tsv.gz" "$P/rsid.idx.part"
mv "$P/rsid.idx.part" "$P/rsid.idx"
echo "PHASE done" >&2
ls -la "$P"
