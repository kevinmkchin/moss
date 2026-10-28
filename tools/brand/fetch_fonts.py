# Downloads open-source fonts that have a lambda, for lambdas.py: every Google Fonts family with Greek (each style
# subset to the one glyph, through the CSS API) and rarer ones from CTAN (Greek Font Society, Computer Modern, Euler,
# GNU FreeFont, Junicode, Libertinus, STIX). Licenses: OFL, GPL with the font exception, GUST.
#   python -I fetch_fonts.py <empty download folder>
import json, os, re, sys, urllib.parse, urllib.request, zipfile, io

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
UA = {"User-Agent": "Mozilla/5.0 (Windows NT 6.1) AppleWebKit/534.30 (KHTML, like Gecko) Safari/534.30"}   # an old Safari: the CSS API answers with TTF

def get(url, headers=None):
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers or {}), timeout=60) as r:
        return r.read()

def safe(name): return re.sub(r"[^A-Za-z0-9_.-]+", "_", name)

# --- Google Fonts: every family with the Greek subset, every style, just the lambda ---
google = os.path.join(out, "google")
os.makedirs(google, exist_ok=True)
meta = json.loads(get("https://fonts.google.com/metadata/fonts"))
for fam in meta["familyMetadataList"]:
    if "greek" not in fam.get("subsets", []): continue
    for style in fam.get("fonts", {}):
        weight, italic = style.rstrip("i"), style.endswith("i")
        spec = f"{fam['family']}:ital,wght@{1 if italic else 0},{weight}"
        url = "https://fonts.googleapis.com/css2?family=" + urllib.parse.quote(spec) + "&text=" + urllib.parse.quote("λ")
        path = os.path.join(google, safe(f"{fam['family']}-{style}") + ".ttf")
        if os.path.exists(path): continue
        try:
            css = get(url, UA).decode("utf-8")
            src = re.search(r"url\((https://[^)]+)\)", css)
            if src: open(path, "wb").write(get(src.group(1)))
        except Exception as e:
            print("google:", fam["family"], style, e)
print("google done")

# --- CTAN: rarer families, whole packages; only their .otf / .ttf are kept ---
ctan = os.path.join(out, "ctan")
os.makedirs(ctan, exist_ok=True)
packages = [
    "fonts/greek/gfs/gfsartemisia", "fonts/greek/gfs/gfsbaskerville", "fonts/greek/gfs/gfsbodoni",
    "fonts/greek/gfs/gfscomplutum", "fonts/greek/gfs/gfsdidot", "fonts/greek/gfs/gfsneohellenic",
    "fonts/greek/gfs/gfsporson", "fonts/greek/gfs/gfssolomos", "fonts/greek/gfs/gfsdidotclassic",
    "fonts/cm-unicode", "fonts/euler-math", "fonts/gnu-freefont", "fonts/junicode", "fonts/libertinus-fonts",
    "fonts/stix2-otf", "fonts/newcomputermodern", "fonts/gentium-sil", "fonts/oldstandard", "fonts/cormorantgaramond",
    "fonts/ebgaramond", "fonts/garamond-math", "fonts/fira-math", "fonts/kerkis", "fonts/greek/kerkis",
]
for pkg in packages:
    name = pkg.split("/")[-1]
    try:
        data = get(f"https://mirrors.ctan.org/{pkg}.zip")
        z = zipfile.ZipFile(io.BytesIO(data))
        n = 0
        for member in z.namelist():
            if member.lower().endswith((".otf", ".ttf")) and ".." not in member:
                open(os.path.join(ctan, safe(name + "-" + os.path.basename(member))), "wb").write(z.read(member))
                n += 1
        print("ctan:", name, n, "fonts")
    except Exception as e:
        print("ctan:", name, e)
