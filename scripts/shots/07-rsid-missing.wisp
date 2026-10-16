# rsID without the pack: grey row, download card idle, then mid-download.
# Mid-download needs a throttled local server:  python3 scripts/shots/slow_server.py 8766 data/packs/rsid &
#   WISP_DATA_BASE_URL=http://127.0.0.1:8766 build/wisp --script scripts/shots/07-rsid-missing.wisp
args --rsid-pack /tmp/wisp-no-pack/rsid.idx
key ctrl+k
type rs4680
wait 100
shotnow 19-rsid-pack-missing-row
click 800 76
wait 100
shotnow 20-download-card-idle
click 646 392
wait 1500
shotnow 21-download-card-mid-download
