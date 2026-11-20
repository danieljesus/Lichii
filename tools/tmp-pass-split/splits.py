import re,sys
exec(open(__import__('os').path.join(__import__('os').path.dirname(__file__), 'segagg.py')).read().split("game=[")[0])
game=[segs for t,segs in frames if t>20 and len(segs)>50]; n=len(game)
spl=0; hp=0
for f in game:
    ps=[s for s in f if s['kind']=='pass']; hp+=len(ps)
    spl+=sum(1 for a,b in zip(ps,ps[1:]) if a['fb']==b['fb'])
print(f"gpu {sum(s['ms'] for f in game for s in f)/n:6.2f} ms/fr  host passes {hp/n:5.1f}  self-splits {spl/n:5.1f}/fr")
