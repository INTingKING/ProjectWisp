# Gene search: Ctrl+K focus, USP17L with +N more, arrow selection, expanded list, +N copies group, jump
key ctrl+k
type USP17L
wait 100
shotnow 09-search-usp17l-more-row
key down
key down
key down
wait 50
shotnow 10-search-arrow-selection
click 800 262
wait 100
shotnow 11-search-more-expanded
key escape
key ctrl+k
type USP17L1
wait 100
shotnow 12-search-copies-row
click 800 150
wait 100
shotnow 13-search-group-sticky
key escape
key ctrl+k
type COMT
key enter
wait 600
shotnow 14-search-jump-comt-outline
