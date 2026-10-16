#!/usr/bin/env python3
"""Build rsid.idx from rsid.tsv.gz (rs_number, chrom, pos1, ref, alts; sorted by rs number).
Layout (little endian): b"WISPRS1\\0", u32 nChrom, u32 nRec, u64 recOff, u64 blobOff, then nChrom x (u8 len, name),
records at recOff: 16 bytes each (u32 rs, u32 pos1, u8 chrom, u8 refLen, u8 altLen, u8 0, u32 blobOffset),
allele blob at blobOff (ref bytes then alt bytes, alleles truncated to 255). The app binary-searches records on disk."""
import gzip, struct, sys

src, dst = sys.argv[1], sys.argv[2]
chroms, cidx = [], {}
recs = bytearray()
blob = bytearray()
n = 0
last = -1
with gzip.open(src, "rt") as f:
    for line in f:
        rs, ch, pos, ref, alt = line.rstrip("\n").split("\t")
        rs = int(rs)
        if rs == last:
            continue
        last = rs
        if ch not in cidx:
            cidx[ch] = len(chroms); chroms.append(ch)
        ref, alt = ref[:255].encode(), alt[:255].encode()
        recs += struct.pack("<IIBBBBI", rs, int(pos), cidx[ch], len(ref), len(alt), 0, len(blob))
        blob += ref + alt
        n += 1
names = b"".join(struct.pack("<B", len(c)) + c.encode() for c in chroms)
hdr = 8 + 4 + 4 + 8 + 8
recOff = hdr + len(names)
recOff = (recOff + 15) // 16 * 16
blobOff = recOff + len(recs)
with open(dst, "wb") as o:
    o.write(b"WISPRS1\0" + struct.pack("<IIQQ", len(chroms), n, recOff, blobOff) + names)
    o.write(b"\0" * (recOff - hdr - len(names)))
    o.write(recs); o.write(blob)
print(f"{dst}: {n} rsIDs, {len(chroms)} chroms, {blobOff + len(blob)} bytes")
