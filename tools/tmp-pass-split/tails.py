# TEMP: submission tails by kind, and what submitted the ones without a render pass.
# python3 tails.py eden_log.txt
import collections, os, sys
exec(open(os.path.join(os.path.dirname(__file__), 'segagg.py')).read().split("game=[")[0])
game = [segs for t, segs in frames if t > 20 and len(segs) > 50]
n = len(game)
kinds = collections.Counter(); kind_ms = collections.Counter()
sites = collections.Counter(); site_ms = collections.Counter()
total = 0.0
for f in game:
    total += sum(s['ms'] for s in f)
    subs = collections.defaultdict(list)
    for s in f:
        subs[s['sub']].append(s)
    for ss in subs.values():
        tail = [s for s in ss if s['kind'] == 'tail'][0]
        has_pass = any(s['kind'] == 'pass' for s in ss)
        empty = tail['draws'] == 0 and tail['disp'] == 0
        k = ('after a pass' if has_pass else 'submission with no pass') + (', empty' if empty else ', with dispatches')
        kinds[k] += 1; kind_ms[k] += tail['ms']
        if not has_pass:
            key = f"{k}: submitted by {tail['why']} {tail['at']}, first shader {tail['s0']}"
            sites[key] += 1; site_ms[key] += tail['ms']
print(f"frames {n}, timed {total / n:.2f} ms a frame")
for k in sorted(kinds, key=lambda k: -kind_ms[k]):
    print(f"  {k:42s} {kinds[k] / n:5.1f}/fr  {kind_ms[k] / n:5.2f} ms/fr  {kind_ms[k] / kinds[k]:.3f} ms each")
print("submissions with no pass, by what submitted them and their first shader:")
for k in sorted(sites, key=lambda k: -site_ms[k])[:20]:
    print(f"  {site_ms[k] / n:5.2f} ms/fr {sites[k] / n:5.1f}/fr  {k}")
