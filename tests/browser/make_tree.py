# The browser test's folder: a few folders and files with fixed dates (the captures don't depend on the day),
# a real PNG and a text file for the preview.
import os, struct, time, zlib

def stamp(path, y, mo, d, h=12):
    t = time.mktime((y, mo, d, h, 0, 0, 0, 0, -1))
    os.utime(path, (t, t))

def png(w, h):
    rows = b''.join(b'\0' + bytes(c for x in range(w) for c in (x * 255 // w, y * 255 // h, 160, 255)) for y in range(h))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')

readme = '# tree\n\nA folder for the browser test.\n\n\tIndented with a tab.\nLast line.\n'
root = 'tree'
files = {
    'alpha/inner.txt': 120, 'alpha/index.html': 4200, 'alpha/deep/notes.md': 64,
    'beta/data.bin': 3 * 1024 * 1024, 'gamma/.keep': 0,
    'readme.md': readme.encode(), 'build.bat': 900, 'image.png': png(320, 200), '.hidden': 10,
}
for rel, content in files.items():
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(content if isinstance(content, bytes) else b'x' * content)
for i, rel in enumerate(sorted(files)):
    stamp(os.path.join(root, rel), 2025, 1 + i % 12, 1 + i)
for d in ['alpha/deep', 'alpha', 'beta', 'gamma']:
    stamp(os.path.join(root, d), 2026, 6, 15, 9)   # the headless clock's today
os.makedirs('outside', exist_ok=True)
with open('outside/drop_me.txt', 'w') as f:
    f.write('dropped in\n')
stamp('outside/drop_me.txt', 2025, 3, 3)
print('tree-made')
