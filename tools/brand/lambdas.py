# Every installed font with a lambda (U+03BB): each one's lambda in lime on Moss, on one sheet, for picking the icon's.
#   python lambdas.py <out.png> [font folder ...]
import os, sys, glob
from fontTools.ttLib import TTFont, TTCollection
from PIL import Image, ImageDraw, ImageFont

BG, LIME, LABEL = (0x14, 0x1A, 0x16), (0xA3, 0xD9, 0x77), (0xB8, 0xC0, 0xB8)
folders = sys.argv[2:] or [r"C:\Windows\Fonts", os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\Windows\Fonts")]
found = []
for folder in folders:
    for path in sorted(glob.glob(os.path.join(folder, "*"))):
        if not path.lower().endswith((".ttf", ".otf", ".ttc", ".woff")): continue
        try:
            fonts = TTCollection(path).fonts if path.lower().endswith(".ttc") else [TTFont(path, lazy=True)]
        except Exception:
            continue
        for index, f in enumerate(fonts):
            try:
                cmap = f.getBestCmap() or {}
                if 0x3BB not in cmap: continue
                name = f["name"].getDebugName(4) or os.path.basename(path)
            except Exception:
                continue
            found.append((name, path, index))

seen = set()
cells = []
for name, path, index in found:
    if name in seen: continue
    seen.add(name)
    try:
        font = ImageFont.truetype(path, 150, index=index)
    except Exception:
        continue
    cell = Image.new("RGB", (220, 240), BG)
    d = ImageDraw.Draw(cell)
    box = d.textbbox((0, 0), "\u03bb", font=font)
    w, h = box[2] - box[0], box[3] - box[1]
    if w <= 0 or h <= 0: continue
    d.text((110 - w / 2 - box[0], 105 - h / 2 - box[1]), "\u03bb", font=font, fill=LIME)
    d.text((8, 216), name[:30], fill=LABEL)
    cells.append(cell)

cols, per = 10, 200   # a sheet per 200 fonts: <out>.png, <out>_2.png, ...
for page in range(0, len(cells), per):
    part = cells[page:page + per]
    rows = (len(part) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * 222, rows * 242), (0x3A, 0x3A, 0x3A))
    for i, c in enumerate(part):
        sheet.paste(c, ((i % cols) * 222, (i // cols) * 242))
    stem, ext = os.path.splitext(sys.argv[1])
    sheet.save(sys.argv[1] if page == 0 else f"{stem}_{page // per + 1}{ext}")
print(len(cells), "fonts")
