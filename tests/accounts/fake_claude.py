# A stand-in for claude in tests: names its tab, says it's here, and waits like an agent at its prompt. Registered
# like Claude Code, in its own CLAUDE_CONFIG_DIR (sessions\<pid>.json), with the session it resumes or claims.
import json, os, sys
for flag in ('--resume', '--session-id'):
    if flag in sys.argv and os.environ.get('CLAUDE_CONFIG_DIR'):
        folder = os.path.join(os.environ['CLAUDE_CONFIG_DIR'], 'sessions')
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, '%d.json' % os.getpid()), 'w') as f:
            json.dump({'pid': os.getpid(), 'sessionId': sys.argv[sys.argv.index(flag) + 1], 'cwd': os.getcwd()}, f, separators=(',', ':'))   # compact, as Claude Code writes it
sys.stdout.write("\x1b]0;fake claude\x07fake-claude ready\r\n> ")
sys.stdout.flush()
for line in sys.stdin:
    sys.stdout.write("fake-claude heard: " + line.strip() + "\r\n> ")
    sys.stdout.flush()
