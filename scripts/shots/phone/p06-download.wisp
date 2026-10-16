# First run on a phone: no data -> download screen; core tracks download (throttled local server), then the map.
# Needs WISP_DATA_BASE_URL pointing at a server with the release files (run_phone.sh sets this up).
args --phone --data /tmp/wisp-phone-data
shot p16-first-run-download
tap 95 280
wait 1500
shotnow p17-core-downloading
idle
wait 200
shot p18-core-installed-map
tap 336 24
wait 100
tap 230 128
wait 100
shot p19-downloads-optional
