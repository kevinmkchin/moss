# Compares a test set's out/ with its reference/: every PNG (pixels that differ, max error) and the
# layout/tabs dumps (out/dumps.txt). Prints one line per file; exit 1 when anything differs.
#   python tools/compare_out.py tests/<set> [max_pixels_off]
import os, sys
from PIL import Image, ImageChops

root = sys.argv[1]
allowed = int(sys.argv[2]) if len(sys.argv) > 2 else 0
out, ref = os.path.join(root, "out"), os.path.join(root, "reference")
if not os.path.isdir(ref):
    print("no reference: run.sh bless")
    sys.exit(0)
bad = False
for name in sorted(os.listdir(ref)):
    a, b = os.path.join(out, name), os.path.join(ref, name)
    if not os.path.exists(a):
        print(f"{name}: MISSING")
        bad = True
    elif name.endswith(".png"):
        ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
        if ia.size != ib.size:
            print(f"{name}: DIFFERENT size {ia.size} vs {ib.size}")
            bad = True
            continue
        diff = ImageChops.difference(ia, ib).convert("L")
        count = sum(1 for v in diff.getdata() if v)
        if count > allowed:
            print(f"{name}: DIFFERENT, {count} pixels, max {diff.getextrema()[1]}")
            bad = True
        else:
            print(f"{name}: same" + (f" ({count} pixels off)" if count else ""))
    else:
        if open(a, encoding="utf-8").read() != open(b, encoding="utf-8").read():
            print(f"{name}: DIFFERENT (diff {a} {b})")
            bad = True
        else:
            print(f"{name}: same")
sys.exit(1 if bad else 0)
