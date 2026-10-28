# Prints what the terminal test looks at: 16 colors, 256 and RGB, attributes, wide characters and emoji.
import sys
sys.stdout.reconfigure(encoding="utf-8")
print(" ".join(f"\x1b[{30+i}m{i}\x1b[0m" for i in range(8)) + " " + " ".join(f"\x1b[{90+i}m{i+8}\x1b[0m" for i in range(8)))
print("".join(f"\x1b[48;5;{i}m " for i in range(16, 64)) + "\x1b[0m")
print("".join(f"\x1b[48;2;{i*8};{255-i*8};128m " for i in range(32)) + "\x1b[0m")
print("\x1b[1mbold\x1b[0m \x1b[3mitalic\x1b[0m \x1b[4munderline\x1b[0m \x1b[9mstrike\x1b[0m \x1b[7minverse\x1b[0m \x1b[2mdim\x1b[0m")
print("wide: 日本語 한국어 中文 | emoji: 😀 🎉 👍🏽 👨‍👩‍👧 🇯🇵 | marks: é ñ ü | box: ┌─┐└─┘")
print("colors-done")
