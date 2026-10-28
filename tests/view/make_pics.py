# The view test's folder: three pictures (gradients of different sizes), a text file and a non-picture.
import os, struct, zlib

def png(w, h, tint):
    rows = b''.join(b'\0' + bytes(c for x in range(w) for c in (x * 255 // w, y * 255 // h, tint, 255)) for y in range(h))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')

os.makedirs('pics', exist_ok=True)
for name, w, h, tint in [('a1.png', 320, 200, 40), ('a2.png', 200, 300, 160), ('a10.png', 640, 360, 255)]:
    open(os.path.join('pics', name), 'wb').write(png(w, h, tint))
open(os.path.join('pics', 'notes.txt'), 'w').write('notes for the view test\n')
open(os.path.join('pics', 'data.bin'), 'wb').write(b'\0' * 100)
print('pics-made')
