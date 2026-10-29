# Two Claude Code accounts for the accounts test, in the run's folder: the server's own (default) config folder cfg1
# (CLAUDE_CONFIG_DIR, run.sh points it there) with a made-up Max login, and cfg2, the "work" account, with a made-up
# Enterprise one. accounts.txt (MOSS_ACCOUNTS_FILE) lists work, gives it the Dusk theme, and keeps dev\ for it.
# cfg2 has a session in dev\proj the sidebar lists. `make_accounts.py leak` copies the work login into cfg1.
import json, os, re, sys

work = os.getcwd()
cfg1, cfg2 = os.environ['CLAUDE_CONFIG_DIR'], os.path.join(work, 'cfg2')

def login(cfg, email, org, plan):
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, '.claude.json'), 'w') as f:
        json.dump({'oauthAccount': {'emailAddress': email, 'organizationName': org}}, f, indent=2)
    with open(os.path.join(cfg, '.credentials.json'), 'w') as f:
        json.dump({'claudeAiOauth': {'accessToken': 'not-a-token', 'subscriptionType': plan}}, f)

if sys.argv[1:] == ['leak']:
    login(cfg1, 'second@example.com', 'Second Org', 'enterprise')
    print('leaked')
    sys.exit(0)

for d in ('dev\\proj', 'home\\proj'):
    os.makedirs(os.path.join(work, d), exist_ok=True)
login(cfg1, 'first@example.com', 'First Org', 'max')
with open(os.path.join(cfg1, 'settings.json'), 'w') as f:   # what a new account copies (and not the login)
    f.write('{}\n')
os.makedirs(os.path.join(cfg1, 'skills', 'hello'), exist_ok=True)
with open(os.path.join(cfg1, 'skills', 'hello', 'SKILL.md'), 'w') as f:
    f.write('# hello\n')
login(cfg2, 'second@example.com', 'Second Org', 'enterprise')
with open(os.environ['MOSS_ACCOUNTS_FILE'], 'w') as f:
    f.write('# the accounts test\naccount work = %s\ntheme work = Dusk\nfolder %s = work\n' % (cfg2, os.path.join(work, 'dev')))

sid = '77777777-7777-4777-8777-777777777777'
cwd = os.path.join(work, 'dev', 'proj')
folder = os.path.join(cfg2, 'projects', re.sub('[^A-Za-z0-9]', '-', cwd))
os.makedirs(folder, exist_ok=True)
with open(os.path.join(folder, sid + '.jsonl'), 'w') as f:
    f.write(json.dumps({'type': 'user', 'cwd': cwd, 'sessionId': sid, 'message': {'role': 'user', 'content': 'Hello from the second account'}}, separators=(',', ':')) + '\n')
    f.write(json.dumps({'type': 'assistant', 'cwd': cwd, 'sessionId': sid, 'message': {'role': 'assistant', 'model': 'claude-sonnet-5-5', 'content': []}}, separators=(',', ':')) + '\n')
print('accounts-made')
