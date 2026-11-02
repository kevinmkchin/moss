# The preview test's folder: markdown, a C file, a picture, a binary file and a long text file.
import os, struct, zlib

def png(w, h, tint):
    rows = b''.join(b'\0' + bytes(c for x in range(w) for c in (x * 255 // w, y * 255 // h, tint, 255)) for y in range(h))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')

os.makedirs('files', exist_ok=True)
open(os.path.join('files', 'a_notes.md'), 'w', newline='\n').write(
    '# Field notes\n\nA *preview* of **markdown**, drawn as the chat view draws a reply.\n\n'
    '## A list\n\n- oak, `quercus`\n- ash\n- rowan\n\n```c\nint main(void) { return 0; }\n```\n\n'
    '| tree | height |\n|---|---|\n| oak | 30 m |\n| ash | 25 m |\n\n> a quote, in italics\n')
open(os.path.join('files', 'b_code.c'), 'w', newline='\n').write(
    '#include <stdio.h>\n\n// prints its arguments\nint main(int argc, char **argv)\n{\n\tfor (int i = 1; i < argc; i++)\n'
    '\t\tprintf("%s\\n", argv[i]);\n\treturn 0;\n}\n')
open(os.path.join('files', 'c_pic.png'), 'wb').write(png(320, 200, 90))
open(os.path.join('files', 'd_blob.bin'), 'wb').write(b'\0\1\2' * 100)
open(os.path.join('files', 'e_long.log'), 'w', newline='\n').write(''.join('line %d of the log\n' % i for i in range(1, 501)))
print('files-made')
