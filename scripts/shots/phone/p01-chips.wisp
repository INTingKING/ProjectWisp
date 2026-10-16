# Phone (360x780 dp, rendered 1080x2340 at 3x): chip picker; swipe the chips left, tap chr7 -> whole-chromosome view
args --phone
shot p01-start-chr1
swipe 300 80 40 80 300
wait 100
tap 116 80
wait 100
shot p02-chip-tapped-chr7
