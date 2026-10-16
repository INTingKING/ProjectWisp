import gzip,collections
def load(it):
    d=collections.defaultdict(list)
    for c,s,e in it: d[c].append((int(s),int(e)))
    for c in d:
        v=sorted(d[c]);m=[]
        for s,e in v:
            if m and s<=m[-1][1]: m[-1][1]=max(m[-1][1],e)
            else: m.append([s,e])
        d[c]=m
    return d
def tot(d): return sum(e-s for v in d.values() for s,e in v)
def sub(a,b):
    r={}
    for c,v in a.items():
        bb=b.get(c,[]);j=0;o=[]
        for s,e in v:
            while j<len(bb) and bb[j][1]<=s: j+=1
            k=j;cur=s
            while k<len(bb) and bb[k][0]<e:
                if bb[k][0]>cur:o.append([cur,bb[k][0]])
                cur=max(cur,bb[k][1]);k+=1
            if cur<e:o.append([cur,e])
        r[c]=o
    return r
FT='exon'
T=sum(int(l.split()[1]) for l in open('hs1.fa.bgz.fai'))
def gff():
    for l in gzip.open('chm13v2.0_RefSeq_Liftoff_v5.3.gff.gz','rt'):
        f=l.split('\t')
        if len(f)>4 and f[2]==FT: yield f[0],int(f[3])-1,f[4]
def bed(p,skip=False):
    for l in open(p):
        if l[0] in '#tb':continue
        f=l.split('\t');yield f[0],f[1],f[2]
genes=load(gff());ccre=load(bed('ccre.hs1.bed'))
known=load([(c,s,e) for d in (genes,ccre) for c,v in d.items() for s,e in v])
rep=sub(load(bed('chm13v2.0_RepeatMasker_4.1.2p1.2022Apr14.bed')),known)
uniq=load(bed('chm13v2-unique_to_hg38.bed'))
k,i=tot(known),tot(rep);n=T-k-i
for name,x in (('genes',tot(genes)),('ccre',tot(ccre)),('known',k),('identified',i),('nothing',n),('notInGRCh38',tot(uniq))):
    print(f'{name} {x} {100*x/T:.2f}%')
print('total',T)
FT='gene';G=load(gff())
intr=sub(G,known);rep2=sub(rep,G)
a,b=tot(intr),tot(rep2);print(f'introns {100*a/T:.2f}% repeats {100*b/T:.2f}% nothing {100*(T-k-a-b)/T:.2f}%')
