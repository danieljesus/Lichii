import re, collections, sys
L=sys.argv[1]
frames=[]; cur=[]
rx=re.compile(r'TEMP seg (\d+) ([\d.]+)ms sub=(\d+) (pass|tail) (\d+)x(\d+) draws=(\d+) disp=(\d+) end=(\S+) at=(\S*) fb=(\S+) clears=(\d+) images=(\d+) s0=(\S+)')
for line in open(L, errors='replace'):
    m=rx.search(line)
    if m:
        g=m.groups(); cur.append(dict(ms=float(g[1]),sub=int(g[2]),kind=g[3],res=f"{g[4]}x{g[5]}",draws=int(g[6]),disp=int(g[7]),why=g[8],at=g[9],fb=g[10],clears=int(g[11]),images=int(g[12]),s0=g[13])); continue
    m=re.search(r'DKB gpu_passes renderer_frame=\d+ passes=\d+.* timed_ms=([\d.]+)', line)
    if m:
        frames.append((float(m.group(1)), cur)); cur=[]
game=[segs for t,segs in frames if t>20 and len(segs)>50]
n=len(game); print(f"frames {n}, segs/frame {sum(map(len,game))/n:.0f}, ms/frame {sum(s['ms'] for f in game for s in f)/n:.2f}")
# same-fb reopen
for f in game:
    passes=[s for s in f if s['kind']=='pass']
    for a,b in zip(passes, passes[1:]):
        a['reopen']= (a['fb']==b['fb'])
def table(title, pred, key):
    agg=collections.defaultdict(lambda:[0,0.0,0])
    for f in game:
        for s in f:
            if pred(s):
                k=key(s); a=agg[k]; a[0]+=1; a[1]+=s['ms']; a[2]+= 1 if s.get('reopen') else 0
    tot_c=sum(v[0] for v in agg.values())/n; tot_ms=sum(v[1] for v in agg.values())/n
    print(f"\n== {title}: {tot_c:.1f} segs/frame, {tot_ms:.2f} ms/frame")
    for k,(c,ms,r) in sorted(agg.items(), key=lambda kv:-kv[1][1])[:16]:
        print(f"  {ms/n:6.2f} ms {c/n:6.1f}/fr  reopen-same-fb {r/n:5.1f}  {k}")
small=lambda s: s['ms']<0.3
table("all segments by end cause", lambda s: True, lambda s: f"{s['kind']} {s['why']} {s['at']}")
table("small (<0.3 ms) by end cause", small, lambda s: f"{s['kind']} {s['why']} {s['at']}")
table("empty (no draw/dispatch) by end cause", lambda s: s['draws']==0 and s['disp']==0, lambda s: f"{s['kind']} {s['why']} {s['at']} clears={s['clears']}")
table("small by resolution", small, lambda s: s['res'])
