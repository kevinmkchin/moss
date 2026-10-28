# The cloud test's folder: a local folder, and a folder and a file marked offline (as online-only cloud placeholders
# are), so the browser shows them as cloud entries and never measures inside them.
import ctypes, os, time

FILE_ATTRIBUTE_OFFLINE = 0x1000

def stamp(path):
    t = time.mktime((2025, 5, 5, 12, 0, 0, 0, 0, -1))
    os.utime(path, (t, t))

for rel, size in {'cloud/local/a.bin': 1000, 'cloud/online/b.bin': 5000, 'cloud/online.txt': 300}.items():
    os.makedirs(os.path.dirname(rel), exist_ok=True)
    with open(rel, 'wb') as f:
        f.write(b'x' * size)
    stamp(rel)
for d in ['cloud/local', 'cloud/online']:
    stamp(d)
for rel in ['cloud/online', 'cloud/online.txt']:
    attrs = ctypes.windll.kernel32.GetFileAttributesW(rel)
    ctypes.windll.kernel32.SetFileAttributesW(rel, attrs | FILE_ATTRIBUTE_OFFLINE)
print('cloud-made')
