# Prints MB/s per terminal and file from tools/bench/results.txt (lines: NAME FILE T0 T1, cmd's %time%).
import os, collections

def seconds(t):
    h, m, s = t.strip().replace(',', '.').split(':')
    return int(h) * 3600 + int(m) * 60 + float(s)

runs = collections.defaultdict(list)
for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results.txt')):
    parts = line.split()
    if len(parts) < 4:
        continue
    name, file, t0, t1 = parts[0], parts[1], parts[-2], parts[-1]
    dt = seconds(t1) - seconds(t0)
    if dt < 0:
        dt += 86400
    runs[(name, file)].append(dt)
for (name, file), times in sorted(runs.items()):
    best = min(times)
    print('%-10s %-10s best %6.2f s  %7.1f MiB/s  (%d runs)' % (name, file, best, 64 / best, len(times)))
