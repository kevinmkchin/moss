# Enters the alternate screen, draws, waits for Enter, leaves.
import sys
sys.stdout.write("\x1b[?1049h\x1b[2J\x1b[H\x1b[44m ALT SCREEN \x1b[0m\r\nalt-ready\r\n")
sys.stdout.flush()
input()
sys.stdout.write("\x1b[?1049l")
print("alt-done")
