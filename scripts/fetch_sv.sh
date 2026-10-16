#!/usr/bin/env bash
# Structural variants from the HPRC v1.1 Minigraph-Cactus pangenome (CHM13 coordinates, 3.0 GB download).
# Keeps sites only (no genotypes), one ALT per record, TYPE ins/del/complex with |LEN| >= 50 plus INV-flagged
# records; writes data/tracks/sv.hprc.vcf.gz (+ .tbi) and deletes the raw download afterwards.
set -euo pipefail
D="${1:-$(dirname "$0")/../data}"; D="$(cd "$D" && pwd)"; mkdir -p "$D/tracks"
URL=https://s3-us-west-2.amazonaws.com/human-pangenomics/pangenomes/freeze/freeze1/minigraph-cactus/hprc-v1.1-mc-chm13/hprc-v1.1-mc-chm13.vcfbub.a100k.wave.vcf.gz
RAW="$D/hprc-v1.1-mc-chm13.vcfbub.a100k.wave.vcf.gz"
OUT="$D/tracks/sv.hprc.vcf.gz"
command -v bcftools >/dev/null || { echo "bcftools missing (apt install bcftools / brew install bcftools)"; exit 1; }
[ -s "$OUT.tbi" ] && { echo "already built: $OUT"; exit 0; }
avail_kb=$(df -Pk "$D" | awk 'NR==2{print $4}')
[ "$avail_kb" -gt $((8*1024*1024)) ] || { echo "need ~8 GB free in $D (have $((avail_kb/1024/1024)) GB)"; exit 1; }
[ -s "$RAW" ] || curl -fL --retry 3 -C - -o "$RAW" "$URL"
bcftools view -G -Ou "$RAW" \
  | bcftools annotate -x INFO/AT,INFO/CONFLICT -Ou \
  | bcftools norm -m - -Ou \
  | bcftools view -i '((INFO/TYPE="ins" || INFO/TYPE="del" || INFO/TYPE="complex") && abs(INFO/LEN)>=50) || INFO/INV=1' -Oz -o "$OUT"
tabix -f -p vcf "$OUT"
rm -f "$RAW"
ls -la "$OUT" "$OUT.tbi"
