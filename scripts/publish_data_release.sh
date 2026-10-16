#!/usr/bin/env bash
# Publish the prebuilt data files as GitHub Release assets (NOT run automatically; needs `gh auth login`).
# Uses the tag and checksums in assets/data_release.conf and refuses to upload files that don't match them.
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"; source "$R/assets/data_release.conf"
D="${1:-$R/data}"
FILES=("$D/packs/rsid/rsid.idx:$RSID_IDX_SHA256" "$D/tracks/sv.hprc.vcf.gz:$SV_VCF_SHA256" "$D/tracks/sv.hprc.vcf.gz.tbi:$SV_TBI_SHA256")
command -v gh >/dev/null || { echo "gh missing"; exit 1; }
for fs in "${FILES[@]}"; do
  f="${fs%%:*}"; want="${fs##*:}"
  [ -s "$f" ] || { echo "missing $f"; exit 1; }
  got=$(sha256sum "$f" | cut -d' ' -f1)
  [ "$got" = "$want" ] || { echo "checksum mismatch for $f (got $got, conf says $want)"; exit 1; }
done
NOTES="Prebuilt optional data for ProjectWisp (not code).
- rsid.idx: common-variant rsID pack (dbSNP 155 on T2T-CHM13 via UCSC, COMMON=1, ALTs trimmed to those seen in 1000 Genomes). sha256 $RSID_IDX_SHA256
- sv.hprc.vcf.gz (+ .tbi): HPRC v1.1 Minigraph-Cactus CHM13 SVs, sites only, |LEN|>=50 plus INV-flagged. sha256 $SV_VCF_SHA256 / $SV_TBI_SHA256
Each keeps its source licence; see the README data credits."
gh release view "$DATA_TAG" -R "$DATA_REPO" >/dev/null 2>&1 \
  || gh release create "$DATA_TAG" -R "$DATA_REPO" --title "Data $DATA_TAG" --notes "$NOTES"
gh release upload "$DATA_TAG" -R "$DATA_REPO" "$D/packs/rsid/rsid.idx" "$D/tracks/sv.hprc.vcf.gz" "$D/tracks/sv.hprc.vcf.gz.tbi"
echo "published $DATA_BASE_URL/{rsid.idx,sv.hprc.vcf.gz,sv.hprc.vcf.gz.tbi}"
