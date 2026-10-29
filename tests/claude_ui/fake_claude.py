# A stand-in for claude: registered like Claude Code (CLAUDE_CONFIG_DIR\sessions\<pid>.json) with the session it
# resumes, a mode line at the bottom as claude draws one, and it echoes what it hears.
import json, os, sys
if '--resume' in sys.argv and os.environ.get('CLAUDE_CONFIG_DIR'):
    folder = os.path.join(os.environ['CLAUDE_CONFIG_DIR'], 'sessions')
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, '%d.json' % os.getpid()), 'w') as f:
        json.dump({'pid': os.getpid(), 'sessionId': sys.argv[sys.argv.index('--resume') + 1], 'cwd': os.getcwd()}, f, separators=(',', ':'))
sys.stdout.write("\x1b]0;fake claude\x07fake-claude ready\r\n> ")
sys.stdout.flush()
# the mode line at the bottom, drawn again a moment later as claude does after the tab's size settles (its account line)
import threading
def mode_line():
    sys.stdout.write("\x1b7\x1b[999;1H  ⏵⏵ auto mode on (shift+tab to cycle)\x1b8")
    sys.stdout.flush()
threading.Timer(2.0, mode_line).start()
for line in sys.stdin:
    sys.stdout.write("fake-claude heard: " + line.strip() + "\r\n> ")
    sys.stdout.flush()
