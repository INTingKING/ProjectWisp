# Full-screen search from the top-bar icon: empty, typed, result tapped
args --phone
tap 284 24
wait 100
shotnow p13-search-empty
type COMT
wait 100
shotnow p14-search-results
tap 180 80
wait 100
shot p15-search-jumped
