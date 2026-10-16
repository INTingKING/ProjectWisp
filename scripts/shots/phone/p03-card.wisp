# Card bottom sheet: tap pins (96 dp peek with position + pill), swipe the handle up -> half -> full, swipe down -> unpin.
# Long-press = live hover preview.
args --phone --region chr22:20,330,000-20,352,000
tap 210 470
wait 100
shot p06-card-peek
swipe 180 700 180 560 250
wait 100
shot p07-card-half
swipe 180 405 180 150 250
wait 100
shot p08-card-full
swipe 180 80 180 760 250
wait 100
swipe 180 700 180 778 200
wait 100
shot p09-card-swiped-away
hold 120 470
wait 600
shotnow p10-long-press-preview
up
