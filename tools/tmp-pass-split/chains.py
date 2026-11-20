import re, collections, sys
exec(open(sys.argv[2]).read().split("game=[")[0].replace("L=sys.argv[1]","L=sys.argv[1]"))
game=[segs for t,segs in frames if t>20 and len(segs)>50]; n=len(game)
# chains: consecutive passes (ignoring tails) with same fb
chains=collections.Counter(); chain_ms=collections.defaultdict(float); pieces_total=0; guest=0
cause_in_chain=collections.Counter()
for f in game:
    passes=[s for s in f if s['kind']=='pass']
    i=0
    while i<len(passes):
        j=i
        while j+1<len(passes) and passes[j+1]['fb']==passes[i]['fb']: j+=1
        k=j-i+1; guest+=1; pieces_total+=k
        if k>1:
            chains[min(k,20)]+=1; chain_ms[passes[i]['res']]+=sum(p['ms'] for p in passes[i:j+1])
            for p in passes[i:j]: cause_in_chain[f"{p['why']} {p['at']}"]+=1
        i=j+1
print(f"host passes/frame {pieces_total/n:.1f}, guest passes (same-fb chains) {guest/n:.1f}; self-splits/frame {(pieces_total-guest)/n:.1f}")
print("chain length dist (len:count/frame):", {k: round(v/n,1) for k,v in sorted(chains.items())})
print("self-split causes/frame:", [(k, round(v/n,1)) for k,v in cause_in_chain.most_common(8)])
print("ms in split chains by res:", {k: round(v/n,2) for k,v in sorted(chain_ms.items(), key=lambda kv:-kv[1])[:6]})
m=[s for f in game for s in f if s['at']=='vk_graphics_pipeline.cpp:1549']
print(f"mesh-split segments: {len(m)/n:.1f}/fr, draws/seg {sum(s['draws'] for s in m)/len(m):.2f}, disp/seg {sum(s['disp'] for s in m)/len(m):.2f}, res {collections.Counter(s['res'] for s in m).most_common(4)}, images {collections.Counter(s['images'] for s in m).most_common(3)}")
print("mesh-split top s0:", collections.Counter(s['s0'] for s in m).most_common(5))
fl=[s for f in game for s in f if s['at']=='vk_rasterizer.cpp:4216']
print(f"flush-split: {len(fl)/n:.1f}/fr, res {collections.Counter(s['res'] for s in fl).most_common(3)}, draws/seg {sum(s['draws'] for s in fl)/len(fl):.1f}")
bc=[s for f in game for s in f if s['at']=='vk_buffer_cache.cpp:636']
print(f"upload-split: {len(bc)/n:.1f}/fr, res {collections.Counter(s['res'] for s in bc).most_common(3)}, draws/seg {sum(s['draws'] for s in bc)/len(bc):.1f}")
subs=[max(s['sub'] for s in f)+1 for f in game]; print(f"submissions/frame {sum(subs)/n:.1f}")
