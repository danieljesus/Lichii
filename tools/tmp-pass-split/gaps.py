# TEMP: GPU span, busy and idle a frame, and the idle time between submissions by what submitted
# the one before. python3 gaps.py eden_log.txt [min_seconds=120]
# Run with EDEN_TMP_BEGIN_STAMP_BOTTOM=1: a TOP_OF_PIPE begin can be stamped before the previous
# submission ends, which makes gaps look smaller (or negative).
import collections, re, sys
log = sys.argv[1]
start = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0
frames = {}
gaps = collections.defaultdict(list)
for line in open(log, errors='replace'):
    t = re.match(r'^\[\s*([\d.]+)\]', line)
    if not t or float(t.group(1)) < start:
        continue
    m = re.search(r'DKB gpu_frame renderer_frame=(\d+) .*gpu_frame_span_ms=([\d.]+) '
                  r'gpu_submission_sum_ms=[\d.]+ gpu_busy_ms=([\d.]+) measured_submissions=(\d+)', line)
    if m:
        frames[int(m.group(1))] = (float(m.group(2)), float(m.group(3)), int(m.group(4)))
        continue
    m = re.search(r'TEMP gap frame=(\d+) sub=(\d+) (-?[\d.]+)ms after=(\S+) at=(\S*)', line)
    if m:
        gaps[int(m.group(1))].append((float(m.group(3)), f"{m.group(4)} {m.group(5)}"))
# Gameplay frames only: a busy GPU (the same filter as the other scripts' timed > 20 ms)
keep = [f for f, (span, busy, subs) in frames.items() if busy > 20 and f in gaps]
n = len(keep)
if n == 0:
    sys.exit("no sampled gameplay frames with gap lines")
span = sum(frames[f][0] for f in keep) / n
busy = sum(frames[f][1] for f in keep) / n
subs = sum(frames[f][2] for f in keep) / n
gap_total = sum(g for f in keep for g, _ in gaps[f]) / n
print(f"frames {n} (t >= {start:.0f} s): span {span:.2f} ms, busy {busy:.2f} ms, idle {span - busy:.2f} ms, "
      f"{subs:.1f} submissions; gaps between them {gap_total:.2f} ms")
by_site = collections.defaultdict(list)
for f in keep:
    for g, site in gaps[f]:
        by_site[site].append(g)
print("idle after each submission, by what submitted it (ms a frame, count a frame, mean, max):")
for site, v in sorted(by_site.items(), key=lambda kv: -sum(kv[1])):
    print(f"  {sum(v) / n:6.2f} ms {len(v) / n:5.1f}/fr  mean {sum(v) / len(v):.3f}  max {max(v):.3f}  {site}")
# Are the big gaps the same ones every frame? Rank of the three biggest gap sites per frame.
top = collections.Counter()
for f in keep:
    for g, site in sorted(gaps[f], reverse=True)[:3]:
        top[site] += 1
print("sites among each frame's three biggest gaps (frames out of", n, "):")
for site, c in top.most_common(6):
    print(f"  {c:4d}  {site}")

# The GPU thread's waits for the back by site (per second, EDEN_NX2_GPU_PROFILE), the acquire
# report (per 10 s) and the TEMP acquire drain split (per 10 s), averaged over the same window.
sites = collections.defaultdict(lambda: [0.0, 0.0]); site_lines = 0
acq = collections.Counter(); acq_lines = 0
temp = collections.defaultdict(collections.Counter); temp_lines = 0
for line in open(log, errors='replace'):
    t = re.match(r'^\[\s*([\d.]+)\]', line)
    if not t or float(t.group(1)) < start:
        continue
    m = re.search(r'waits for the back by site: (.*?) \|', line)
    if m:
        site_lines += 1
        for name, ms, per in re.findall(r'([a-z ]+?) ([\d.]+) ms \(([\d]+)/s\)', m.group(1)):
            sites[name.strip()][0] += float(ms); sites[name.strip()][1] += float(per)
        continue
    m = re.search(r'Acquires: (\d+) passed without a drain \((\d+) by a queued release\).*?, (\d+) drained .*?(\d+) read elsewhere', line)
    if m:
        acq_lines += 1
        for k, v in zip(('passed', 'by_queued', 'drained', 'elsewhere'), m.groups()):
            acq[k] += int(v)
        continue
    if 'TEMP acquire drains in 10 s' in line:
        temp_lines += 1
        for kind, body in re.findall(r'\| (\w+) (n=[^|]+)', line):
            for k, v in re.findall(r'(\w+)=([\d.]+)', body):
                temp[kind][k] += float(v)
if site_lines:
    print(f"GPU thread waits for the back, per second ({site_lines} reports):")
    for name, (ms, per) in sorted(sites.items(), key=lambda kv: -kv[1][0]):
        print(f"  {name:18s} {ms / site_lines:6.1f} ms/s  {per / site_lines:7.0f}/s")
if acq_lines:
    print(f"acquires per 10 s ({acq_lines} reports): passed {acq['passed'] / acq_lines:.0f} "
          f"(by a queued release {acq['by_queued'] / acq_lines:.0f}), drained {acq['drained'] / acq_lines:.0f}, "
          f"of them elsewhere {acq['elsewhere'] / acq_lines:.0f}")
if temp_lines:
    print(f"TEMP trigger-path acquire drains per 10 s ({temp_lines} reports):")
    for kind, c in temp.items():
        print(f"  {kind:6s} n={c['n'] / temp_lines:8.0f} ms={c['ms'] / temp_lines:6.1f} "
              f"met_before={c['met_before'] / temp_lines:8.0f} changed={c['changed'] / temp_lines:8.0f} "
              f"met_after={c['met_after'] / temp_lines:8.0f} met_by_queued_work={c['met_by_queued_work'] / temp_lines:8.0f} "
              f"ms_over_2us={c['ms_over_2us'] / temp_lines:6.1f}")
