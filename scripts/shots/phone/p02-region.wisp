# Phone region view around COMT (22 kb), then a two-finger pinch-out zooms in around the fingers' centre
args --phone --region chr22:20,330,000-20,352,000
shot p03-region-comt
pinch 190 420 80 240 300
wait 100
shot p04-after-pinch-zoom
swipe 120 420 260 420 300
wait 100
shot p05-after-one-finger-pan
