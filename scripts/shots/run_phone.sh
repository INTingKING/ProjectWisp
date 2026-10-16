#!/usr/bin/env bash
# Phone shot set (portrait 1080x2340 = 360x780 dp at 3x): scripts/shots/run_phone.sh [out_dir] (default /workspace/shots/phone)
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$R"; OUT="${1:-$R/../shots/phone}"; mkdir -p "$OUT"
SRV=/tmp/wisp-phone-srv; rm -rf "$SRV" /tmp/wisp-phone-data; mkdir -p "$SRV"
# flat directory that looks like the GitHub release (asset name = basename), from assets/data_packs.tsv
grep -v '^#' assets/data_packs.tsv | while IFS=$'\t' read -r pack asset path bytes sha; do ln -sf "$(readlink -f "data/$path")" "$SRV/$asset"; done
python3 scripts/shots/slow_server.py 8767 "$SRV" 10000 & SP=$!; trap 'kill $SP' EXIT; sleep 0.5
for s in scripts/shots/phone/*.wisp; do
  echo "== $s"
  WISP_DATA_BASE_URL=http://127.0.0.1:8767 build/wisp --shots-dir "$OUT" --script "$s" 2>&1 | grep -E "wrote|script|error" || true
done
python3 scripts/shots/phone_sheet.py "$OUT"
