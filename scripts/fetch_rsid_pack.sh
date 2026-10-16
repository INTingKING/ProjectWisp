#!/usr/bin/env bash
# OPTIONAL rsID search pack (not part of the base bundle).
# Streams UCSC's dbSNP 155 on T2T-CHM13 v2.0 (~25 GB, never stored on disk) and keeps dbSNP's COMMON=1 variants
# (minor allele >= 1% in at least one 1000 Genomes population, 2+ founders), fields ID, CHROM, POS, REF, ALT only.
# Writes data/packs/rsid/rsid.idx (sorted binary, binary-searched from disk by the app) and rsid.tsv.gz (text copy).
set -euo pipefail
D="${1:-$(dirname "$0")/../data}"; D="$(cd "$D" && pwd)"; P="$D/packs/rsid"; mkdir -p "$P"
URL=https://hgdownload.soe.ucsc.edu/gbdb/hs1/dbSNP155/chm13v2.0_dbSNPv155.vcf.gz
command -v bgzip >/dev/null || { echo "bgzip missing (apt install tabix)"; exit 1; }
[ -s "$P/rsid.idx" ] && { echo "already built: $P/rsid.idx"; exit 0; }
export LC_ALL=C
if [ ! -s "$P/rsid.tsv.gz" ]; then
  command -v bcftools >/dev/null || { echo "bcftools missing (apt install bcftools)"; exit 1; }
  # grep only pre-thins the text stream (header + lines mentioning COMMON); bcftools does the real filter
  curl -fsSL --retry 3 "$URL" | bgzip -d -@4 | grep -E '^#|COMMON' \
    | bcftools view -i 'INFO/COMMON=1' -Ou - \
    | bcftools query -f '%ID\t%CHROM\t%POS\t%REF\t%ALT\n' \
    | awk -F'\t' 'BEGIN{OFS="\t"} substr($1,1,2)=="rs" {print substr($1,3),$2,$3,$4,$5}' \
    | sort -S 1G -n -k1,1 -u | gzip -6 > "$P/rsid.tsv.gz.part"
  mv "$P/rsid.tsv.gz.part" "$P/rsid.tsv.gz"
fi
python3 "$(dirname "$0")/build_rsid_index.py" "$P/rsid.tsv.gz" "$P/rsid.idx"
ls -la "$P"
