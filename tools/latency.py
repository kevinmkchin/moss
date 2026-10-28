# Keystroke latency from MOSS_LATENCY logs (%TEMP%	erm-latency-<pid>.txt from the window and the server).
# For each "key" it finds the first of each later hop and prints per-hop medians and worst cases.
# usage: python tools/latency.py [log folder]   (deletes nothing; clear old logs yourself)
import glob, os, statistics, sys

folder = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TEMP", ".")
events = []
for path in glob.glob(os.path.join(folder, "moss-latency-*.txt")):
    for line in open(path):
        parts = line.split()
        if len(parts) == 2:
            events.append((int(parts[1]), parts[0]))
events.sort()

hops = ["client_send", "server_in", "server_write", "pty_read", "server_frame", "client_wake", "client_cells", "client_present"]
rows = []
keys = [i for i, e in enumerate(events) if e[1] == "key"]
for n, k in enumerate(keys):
    end = keys[n + 1] if n + 1 < len(keys) else len(events)
    t0 = events[k][0]
    row = {}
    i = k
    for hop in hops:
        # a hop this path doesn't have (client_wake is the headless runner's) is skipped
        j = i
        while j < end and events[j][1] != hop:
            j += 1
        if j >= end:
            continue
        i = j
        row[hop] = (events[i][0] - t0) / 1000.0
    if "client_present" in row:
        rows.append(row)

print("%d keys measured (ms after the key reached the window)" % len(rows))
prev = None
for hop in hops:
    vals = [r[hop] for r in rows if hop in r]
    if not vals:
        continue
    step = [r[hop] - (r[prev] if prev else 0) for r in rows if hop in r and (not prev or prev in r)]
    print("%-15s at median %7.2f  p95 %7.2f  max %7.2f   step median %6.2f  max %6.2f" % (
        hop, statistics.median(vals), sorted(vals)[int(len(vals) * 0.95) - 1] if len(vals) > 1 else vals[0], max(vals),
        statistics.median(step), max(step)))
    prev = hop
