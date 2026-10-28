# A stand-in for claude in tests: names its tab, says it's here, and waits like an agent at its prompt.
import sys
sys.stdout.write("\x1b]0;fake claude\x07fake-claude ready\r\n> ")
sys.stdout.flush()
for line in sys.stdin:
    sys.stdout.write("fake-claude heard: " + line.strip() + "\r\n> ")
    sys.stdout.flush()
