#!/usr/bin/env python3
"""Per-bin category summary for the overview (format WISPOV1, little endian).
Per-base priority (same as coverage.py): exon > cCRE > intron(gene body) > repeat > nothing;
notInGRCh38 is an independent overlay.
Layout: b'WISPOV1\\0', u32 bin, u32 nchrom, u64 totals[7] (exon ccre intron repeat nothing notInGRCh38 genome),
  per chrom: u8 namelen, name, u64 length, u32 nbins; then for each chrom, nbins * u16[6]
  (fractions *65535 of exon ccre intron repeat nothing notInGRCh38)."""
import sys, gzip, struct, collections
import numpy as np
D, OUT = sys.argv[1], sys.argv[2]
BIN = 100_000
chroms = [(l.split()[0], int(l.split()[1])) for l in open(f'{D}/hs1.fa.bgz.fai')]
def collect(it):
    d = collections.defaultdict(lambda: ([], []))
    for c, s, e in it:
        a = d[c]; a[0].append(s); a[1].append(e)
    return {c: (np.array(s, np.int64), np.array(e, np.int64)) for c, (s, e) in d.items()}
def gff(ft):
    for l in gzip.open(f'{D}/chm13v2.0_RefSeq_Liftoff_v5.3.gff.gz', 'rt'):
        f = l.split('\t', 6)
        if len(f) > 4 and f[2] == ft: yield f[0], int(f[3]) - 1, int(f[4])
def bed(p):
    for l in open(p):
        if l[0] in '#tb': continue
        f = l.split('\t', 3); yield f[0], int(f[1]), int(f[2])
print('loading...', file=sys.stderr)
layers = [  # painted in increasing priority, value = code
    (4, collect(bed(f'{D}/chm13v2.0_RepeatMasker_4.1.2p1.2022Apr14.bed'))),
    (3, collect(gff('gene'))),
    (2, collect(bed(f'{D}/ccre.hs1.bed'))),
    (1, collect(gff('exon'))),
]
uniq = collect(bed(f'{D}/chm13v2-unique_to_hg38.bed'))
tot = np.zeros(7, np.int64); hdr = []; body = []
for c, L in chroms:
    a = np.full(L, 5, np.uint8)  # 5 = nothing
    for code, d in layers:
        if c in d:
            for s, e in zip(*d[c]): a[s:e] = code
    u = np.zeros(L, np.uint8)
    if c in uniq:
        for s, e in zip(*uniq[c]): u[s:e] = 1
    nb = (L + BIN - 1) // BIN
    idx = np.arange(L) // BIN
    cols = []
    for code in (1, 2, 3, 4, 5):
        m = a == code
        tot[code - 1] += int(m.sum())
        cols.append(np.bincount(idx[m], minlength=nb))
    tot[5] += int(u.sum()); tot[6] += L
    cols.append(np.bincount(idx, weights=u, minlength=nb))
    size = np.minimum(BIN, L - np.arange(nb) * BIN)
    fr = np.stack(cols, 1) / size[:, None]
    body.append(np.round(fr * 65535).astype('<u2').tobytes())
    hdr.append(struct.pack('<B', len(c)) + c.encode() + struct.pack('<QI', L, nb))
    del a, u, idx
    print(c, file=sys.stderr)
with open(OUT, 'wb') as f:
    f.write(b'WISPOV1\0' + struct.pack('<II', BIN, len(chroms)) + struct.pack('<7Q', *tot))
    for h in hdr: f.write(h)
    for b in body: f.write(b)
n = ['exon', 'ccre', 'intron', 'repeat', 'nothing', 'notInGRCh38']
print(' '.join(f'{k}={100*v/tot[6]:.2f}%' for k, v in zip(n, tot)))
