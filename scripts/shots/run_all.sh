#!/usr/bin/env bash
# Re-render the review shot set: scripts/shots/run_all.sh [out_dir] (default /workspace/shots/review)
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$R"; OUT="${1:-$R/../shots/review}"; mkdir -p "$OUT"
python3 scripts/shots/slow_server.py 8766 data/packs/rsid 20000 & SP=$!; trap 'kill $SP' EXIT; sleep 0.5
for s in scripts/shots/*.wisp; do
  echo "== $s"
  WISP_DATA_BASE_URL=http://127.0.0.1:8766 build/wisp --shots-dir "$OUT" --script "$s" 2>&1 | grep -E "wrote|script|error" || true
done
rm -rf /tmp/wisp-no-pack
python3 scripts/shots/contact_sheet.py "$OUT"
