# The background test's image: a 640x360 diagonal gradient with a light disc in the middle (so cropping and the
# span across panes show in the capture).
import struct, zlib

w, h = 640, 360
def pixel(x, y):
    dx, dy = x - w / 2, y - h / 2
    if dx * dx + dy * dy < 90 * 90:
        return (240, 220, 120)
    return (x * 255 // w, 60, y * 255 // h)
rows = b''.join(b'\0' + bytes(c for x in range(w) for c in pixel(x, y)) for y in range(h))
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
with open('bg.png', 'wb') as f:
    f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
print('bg-made')
