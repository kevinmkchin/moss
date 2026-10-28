# Icon concepts in Moss, drawn as shapes (no fonts) at 8x and scaled down; writes one sheet per concept size row.
#   python icon_concepts.py <out folder>
import sys, os
from PIL import Image, ImageDraw

BG = (0x14, 0x1A, 0x16)
PANEL = (0x20, 0x28, 0x22)
LIME = (0xA3, 0xD9, 0x77)
SAGE = (0x56, 0x63, 0x5A)
FG = (0xD3, 0xDD, 0xD3)
K = 8   # supersampling

def canvas():
    im = Image.new("RGBA", (1024 * K // 4, 1024 * K // 4), (0, 0, 0, 0))   # 2048 px, design grid 1024 -> *2
    return im, ImageDraw.Draw(im)

def S(v): return int(v * 2)   # design units (1024 grid) to canvas px

def block(d, fill=BG):
    d.rounded_rectangle((0, 0, S(1024) - 1, S(1024) - 1), radius=S(230), fill=fill)

def chevron(d, x, y, h, w, color):
    # ">" as a thick polyline: from (x, y) to (x + w, y + h / 2) to (x, y + h)
    t = h * 0.22
    d.polygon([(S(x), S(y)), (S(x + t * 0.9), S(y)), (S(x + w), S(y + h / 2)), (S(x + t * 0.9), S(y + h)), (S(x), S(y + h)),
               (S(x + w - t * 0.9), S(y + h / 2))], fill=color)

def bar(d, x, y, w, h, color):
    d.rounded_rectangle((S(x), S(y), S(x + w), S(y + h)), radius=S(h * 0.25), fill=color)

def folder(d, x, y, w, h, color, tab_w=0.38, outline=0):
    tab_h = h * 0.16
    r = h * 0.09
    if outline:
        d.rounded_rectangle((S(x), S(y), S(x + w * tab_w), S(y + tab_h * 2)), radius=S(r), outline=color, width=S(outline))
        d.rounded_rectangle((S(x), S(y + tab_h), S(x + w), S(y + h)), radius=S(r), outline=color, width=S(outline))
    else:
        d.rounded_rectangle((S(x), S(y), S(x + w * tab_w), S(y + tab_h * 2)), radius=S(r), fill=color)
        d.rounded_rectangle((S(x), S(y + tab_h), S(x + w), S(y + h)), radius=S(r), fill=color)

def a_folder_prompt():
    # lime folder outline holding a lime ">_": the folder filled lime, then filled again inset in the background
    im, d = canvas(); block(d)
    folder(d, 150, 210, 724, 600, LIME)
    t = 44
    folder(d, 150 + t, 210 + t, 724 - 2 * t, 600 - 2 * t, BG, tab_w=0.36)
    d.rectangle((S(150 + t), S(210 + t + 600 * 0.16 * 2 - t), S(150 + 724 - t), S(210 + t + 600 * 0.16 * 2 - t + 1)), fill=BG)
    d.rectangle((S(150 + t), S(210 + 600 * 0.16 + 0), S(150 + 724 - t), S(210 + 600 * 0.16 + t)), fill=LIME)
    chevron(d, 300, 450, 230, 150, LIME)
    bar(d, 490, 634, 200, 46, LIME)
    return im

def b_split():
    # two panes: a prompt on the left, a file list on the right, a lime rule between
    im, d = canvas(); block(d)
    chevron(d, 140, 400, 230, 150, LIME)
    bar(d, 320, 580, 130, 50, LIME)
    d.rectangle((S(497), S(150), S(527), S(874)), fill=SAGE)
    for i, w in enumerate([260, 200, 230, 170]):
        y = 300 + i * 120
        d.rounded_rectangle((S(600), S(y), S(650), S(y + 50)), radius=S(10), fill=LIME if i < 2 else SAGE)
        bar(d, 680, y + 6, w - 90, 38, FG if i < 2 else SAGE)
    return im

def c_solid_folder():
    # lime folder filled, a dark ">_" cut into it
    im, d = canvas(); block(d)
    folder(d, 130, 200, 764, 640, LIME)
    chevron(d, 280, 450, 230, 150, BG)
    bar(d, 470, 640, 200, 46, BG)
    return im

def d_sprout():
    # ">" prompt whose cursor is a leaf: moss growing from the prompt
    im, d = canvas(); block(d)
    chevron(d, 170, 330, 360, 230, LIME)
    # the leaf: where two circles overlap (a vesica), tilted up from the baseline
    w, h = S(360), S(170)
    a = Image.new("L", (w, h), 0); ImageDraw.Draw(a).ellipse((-w * 0.25, -h * 0.9, w * 1.25, h * 1.0), fill=255)
    b = Image.new("L", (w, h), 0); ImageDraw.Draw(b).ellipse((-w * 0.25, 0, w * 1.25, h * 1.9), fill=255)
    from PIL import ImageChops
    mask = ImageChops.multiply(a, b)
    leaf = Image.new("RGBA", (w, h), LIME + (255,)); leaf.putalpha(mask)
    leaf = leaf.rotate(30, resample=Image.BICUBIC, expand=True)
    im.alpha_composite(leaf, (S(450), S(470)))
    return im

concepts = [("A folder + prompt", a_folder_prompt), ("B split panes", b_split), ("C solid folder", c_solid_folder), ("D sprout prompt", d_sprout)]
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
sizes = [256, 64, 48, 32, 24, 16]
cell = 300
sheet = Image.new("RGB", (cell * len(concepts), 30 + 256 + 20 + 64 + 20), (0x3A, 0x3A, 0x3A))
light = Image.new("RGB", sheet.size, (0xF0, 0xF0, 0xF0))
for n, (name, fn) in enumerate(concepts):
    big = fn()
    big.save(os.path.join(out, name.split()[0] + ".png"))
    for target in (sheet, light):
        dd = ImageDraw.Draw(target)
        dd.text((n * cell + 20, 8), name, fill=(230, 230, 230) if target is sheet else (30, 30, 30))
        x = n * cell + 20
        target.paste(big.resize((256, 256), Image.LANCZOS), (x, 30), big.resize((256, 256), Image.LANCZOS))
        x2 = x
        for s in sizes[1:]:
            small = big.resize((s, s), Image.LANCZOS)
            target.paste(small, (x2, 30 + 256 + 20 + (64 - s)), small)
            x2 += s + 12
sheet.save(os.path.join(out, "icons_dark.png"))
light.save(os.path.join(out, "icons_light.png"))
