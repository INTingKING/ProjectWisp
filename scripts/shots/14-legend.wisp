# Legend panel (280 px, right of the region view): opened via --legend in a region with every track loaded
# (COMT 50 kb: genes, cCREs, repeats, variants, SVs, methylation) at the default 1600x1000; the panel is capped
# at the region view's height and scrolls inside
args --region chr22:20,320,000-20,370,000 --pin chr22:20,341,376 --legend
shot 31-legend-open-all-tracks
# wheel over the panel: the current group header (sticky) stays pinned at the panel top
move 1050 700
wheel -5
wait 50
shotnow 31b-legend-scrolled-sticky-header
