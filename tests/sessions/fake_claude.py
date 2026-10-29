# A stand-in for claude in tests: names its tab, says it's here, and waits like an agent at its prompt. Registered
# like Claude Code (CLAUDE_CONFIG_DIR\sessions\<pid>.json) with the session it resumes, so its tab is a claude tab.
import json, os, sys
if '--resume' in sys.argv and os.environ.get('CLAUDE_CONFIG_DIR'):
    folder = os.path.join(os.environ['CLAUDE_CONFIG_DIR'], 'sessions')
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, '%d.json' % os.getpid()), 'w') as f:
        json.dump({'pid': os.getpid(), 'sessionId': sys.argv[sys.argv.index('--resume') + 1], 'cwd': os.getcwd()}, f, separators=(',', ':'))   # compact, as Claude Code writes it
sys.stdout.write("\x1b]0;fake claude\x07fake-claude ready\r\n> ")
sys.stdout.flush()
for line in sys.stdin:
    sys.stdout.write("fake-claude heard: " + line.strip() + "\r\n> ")
    sys.stdout.flush()
    if line.strip() == 'switch-account':   # another login under it, as a /login elsewhere would leave it
        with open(os.path.join(os.environ['CLAUDE_CONFIG_DIR'], '.claude.json'), 'w') as f:
            json.dump({'oauthAccount': {'emailAddress': 'someone.else@example.com', 'organizationName': 'Other Org'}}, f, indent=2)
    if line.strip() == '/login':           # this claude logs in to another account
        with open(os.path.join(os.environ['CLAUDE_CONFIG_DIR'], '.claude.json'), 'w') as f:
            json.dump({'oauthAccount': {'emailAddress': 'third@example.com', 'organizationName': 'Third Org'}}, f, indent=2)
