# Claude Code sessions for the sidebar test, in CLAUDE_CONFIG_DIR (run.sh points it here): a transcript per session
# in projects\<mangled folder>\<id>.jsonl, dated against the headless clock (2026-06-15 12:00 UTC).
import json, os, re, time, calendar

cfg = os.environ['CLAUDE_CONFIG_DIR']
now = calendar.timegm((2026, 6, 15, 12, 0, 0))
sessions = [
    ('11111111-1111-4111-8111-111111111111', 'term', 'Fix the tab drag drop zones', 2 * 3600),
    ('22222222-2222-4222-8222-222222222222', 'game', 'Profile the frame spikes', 26 * 3600),
    ('33333333-3333-4333-8333-333333333333', 'term', 'Folder sizes use too much CPU', 4 * 86400),
    ('44444444-4444-4444-8444-444444444444', 'clod', 'Tray icons for unread apps', 60 * 86400),
]
for sid, project, prompt, age in sessions:
    cwd = os.path.join(os.getcwd(), 'projects', project)
    os.makedirs(cwd, exist_ok=True)
    folder = os.path.join(cfg, 'projects', re.sub('[^A-Za-z0-9]', '-', cwd))
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, sid + '.jsonl')
    with open(path, 'w') as f:
        f.write(json.dumps({'type': 'user', 'cwd': cwd, 'sessionId': sid, 'message': {'role': 'user', 'content': prompt}}, separators=(',', ':')) + '\n')   # compact, as Claude Code writes it
    os.utime(path, (now - age, now - age))
print('sessions-made')
