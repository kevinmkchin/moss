# Test files for the throughput bench (tools/bench/run.ps1), in the folder given (default: here):
#   plain.txt  64 MiB of ASCII lines, 120 columns
#   color.txt  64 MiB of lines whose colors change every few characters (256-color and 24-bit SGR), the heavy case
import os, random, sys

out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
size = 64 * 1024 * 1024
random.seed(7)
words = [''.join(random.choice('abcdefghijklmnopqrstuvwxyz') for _ in range(random.randint(2, 9))) for _ in range(2000)]

def plain_line():
    s = ''
    while len(s) < 119:
        s += random.choice(words) + ' '
    return s[:119] + '\n'

def color_line():
    parts, n = [], 0
    while n < 119:
        w = random.choice(words)[:119 - n]
        k = random.random()
        if k < 0.4:
            parts.append('\x1b[38;5;%dm' % random.randint(16, 231))
        elif k < 0.7:
            parts.append('\x1b[38;2;%d;%d;%dm' % (random.randint(0, 255), random.randint(0, 255), random.randint(0, 255)))
        elif k < 0.8:
            parts.append('\x1b[48;5;%dm' % random.randint(232, 255))
        else:
            parts.append('\x1b[0m')
        parts.append(w + ' ')
        n += len(w) + 1
    return ''.join(parts) + '\x1b[0m\n'

for name, make in (('plain.txt', plain_line), ('color.txt', color_line)):
    lines = [make() for _ in range(4000)]   # a pool of lines, repeated to the size
    with open(os.path.join(out, name), 'w', newline='') as f:
        written = 0
        i = 0
        while written < size:
            line = lines[i % len(lines)]
            f.write(line)
            written += len(line)
            i += 1
print('bench files made')
