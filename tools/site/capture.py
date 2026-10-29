# Takes the website's screenshots of build\moss.exe into content\ (`build content` runs it), with moss's
# headless mode: no window, fixed clock, exact frames. Everything on screen is made up: a scratch home
# (USERPROFILE: fresh ~\.moss, no shell history), fake Claude Code sessions (CLAUDE_CONFIG_DIR), a fake
# claude (MOSS_CLAUDE = fake_claude.py) and a fake codebase at C:\src\meshkit with fixed git history.
#
#   python tools\site\capture.py [--scene panes|explorer|shell|all] [--keep-scratch]
#
# C:\src is made for the run and removed after (C:\src itself only if this made it); the run refuses
# to start if any of its folders already exist. Scratch lives in build\site-scratch.

import argparse
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MOSS = os.path.abspath(os.path.join(HERE, '..', '..'))
OUT = os.path.join(MOSS, 'content')
SCRATCH = os.path.join(MOSS, 'build', 'site-scratch')

# Headless moss runs on a fake clock: 2026-06-15 12:00 UTC. Session ages are counted back from it.
NOW = 1781524800

# Sessions in the sidebar, newest first: id, folder, title, age in seconds. Ids with a scenario in
# fake_claude.py can be opened into a pane.
SESSIONS = [
    ('6f1c2a90-1d4e-4b7a-9f00-000000000001', 'C:\\src\\game', 'Implement motion matching locomotion', 120),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000002', 'C:\\src\\re', 'Reverse engineer the model and animation formats', 540),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000003', 'C:\\src\\meshkit', 'Fix non-manifold edges after collapse', 1800),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000004', 'C:\\src\\game', 'Add foot IK to the run cycle', 3 * 3600),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000005', 'C:\\src\\re', 'Unpack the audio banks', 7 * 3600),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000006', 'C:\\src\\meshkit', 'Speed up the BVH build', 26 * 3600),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000007', 'C:\\src\\game', 'Inertialization for blend transitions', 2 * 86400),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000008', 'C:\\src\\website', 'Fix the broken RSS feed', 3 * 86400),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000009', 'C:\\src\\dotfiles', 'Clean up my PowerShell profile', 6 * 86400),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000010', 'C:\\src\\meshkit', 'Port the decimator to SIMD', 9 * 86400),
]

MESHKIT = 'C:\\src\\meshkit'
# Its git history, oldest first: date, message, files that commit adds. Fixed dates and author keep the
# hashes and the ll dates the same every run.
MESHKIT_HISTORY = [
    ('2026-05-02T10:14:00', 'Indexed mesh and OBJ loading', ['README.md', 'build.bat', 'src/mesh.h']),
    ('2026-05-09T21:40:00', 'Half-edge connectivity with boundary edges', ['src/halfedge.cpp']),
    ('2026-05-21T19:05:00', 'Quadric error metric simplification', ['src/quadric.h', 'src/heap.h', 'src/simplify.cpp']),
    ('2026-06-03T22:31:00', 'Test that simplified meshes stay manifold', ['tests/test_simplify.cpp']),
    ('2026-06-12T20:12:00', 'Binned SAH BVH build', ['src/bvh.cpp']),
]

# A game project for the explorer: path, size in bytes, date. The big files are only allocated (truncate),
# never written, so they cost no time; the browser measures the folders' sizes from them.
GAME = 'C:\\src\\game'
GAME_FILES = [
    ('assets/models/hero.fbx', 14812000, '2026-06-02'),
    ('assets/models/crate.fbx', 1204000, '2026-05-18'),
    ('assets/textures/hero_albedo.png', 22369000, '2026-06-02'),
    ('assets/textures/hero_normal.png', 21904000, '2026-06-02'),
    ('assets/textures/terrain_splat.png', 67108000, '2026-05-27'),
    ('assets/audio/footsteps.wav', 8430000, '2026-05-30'),
    ('assets/audio/music_theme.ogg', 5902000, '2026-06-08'),
    ('assets/anim/run_turns.bvh', 3210000, '2026-06-14'),
    ('assets/anim/idle.bvh', 640000, '2026-06-11'),
    ('build/game.exe', 4718592, '2026-06-15'),
    ('build/game.pdb', 38420000, '2026-06-15'),
    ('code/main.cpp', 18402, '2026-06-12'),
    ('code/anim/motion_db.cpp', 9731, '2026-06-15'),
    ('code/anim/matcher.cpp', 6204, '2026-06-15'),
    ('code/render/renderer.cpp', 41288, '2026-06-09'),
    ('code/render/shaders.hlsl', 12044, '2026-06-09'),
    ('code/physics.cpp', 22910, '2026-05-21'),
    ('docs/design.md', 7120, '2026-05-14'),
    ('tools/pack_assets.py', 4310, '2026-05-30'),
    ('build.bat', 612, '2026-05-02'),
    ('game.sln', 1843, '2026-05-02'),
    ('README.md', 1204, '2026-06-01'),
    ('.gitignore', 96, '2026-05-02'),
]

# Scenes: output file, window size at 144 dpi, and the script for `moss --headless --script` (see the
# run-headless skill). {out} is the output folder.
SCENES = {
    # Panes and agents, 2x2: Claude at work top left, a shell below it, another session done top right,
    # the files of the folder below that, the sessions sidebar open. Every split first: pane numbers are
    # by place (top left 0, bottom left 1, top right 2, bottom right 3) once the layout is whole.
    'panes': ('screenshot.png', '2400x1400', '''
waitfor λ
cmd split_right
waitfor λ pane=1
cmd split_down
waitfor λ pane=2
cmd toggle_browser
waitlabel README.md
cmd focus_left
wait 2
cmd split_down
waitfor λ pane=1
# bottom left: a little shell work
type ll
key Enter
waitfor tests/ pane=1
type git log --oneline --graph -5
key Enter
waitfor Indexed mesh pane=1
# the server reads the sessions on its own thread, every 5 s
sleep 6000
waitidle
# open a session in a pane: focus it, give the sidebar the keyboard, filter, Enter resumes it there
cmd focus_up
wait 2
cmd sessions
wait 2
type motion matching
wait 2
key Enter
waitfor Searching pane=0 timeout=20000
cmd focus_right
wait 2
cmd sessions
wait 2
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
type animation formats
wait 2
key Enter
waitfor chars.pak pane=2 timeout=20000
# the whole list again, and the keyboard back to the working session
cmd sessions
wait 2
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
key Backspace
wait 2
cursorpane 0
wait 1
lmb down
wait 1
lmb up
sleep 3000
waitidle
capture {out}/screenshot.png
'''),
    # The file explorer: the tab's other face, on a game project, folder sizes measured in the background.
    'explorer': ('screenshot-explorer.png', '2000x700', '''
waitfor λ
cmd sessions
wait 2
cmd sessions
wait 2
type cd /d C:\\src\\game
key Enter
waitfor src\\game main
# the server reads the shell's folder about once a second; the browser opens where it last saw it
sleep 2500
cmd toggle_browser
waitlabel game.sln
sleep 3000
waitidle
capture {out}/screenshot-explorer.png
'''),
    # The shell: cmd + clink in a git repo, unix tools, clink listing git's completions (Alt+=).
    'shell': ('screenshot-shell.png', '1600x800', '''
waitfor λ
cmd sessions
wait 2
cmd sessions
wait 2
type cls
key Enter
waitfor λ
type ll
key Enter
waitfor tests/
type grep -rn --color TODO src
key Enter
waitfor refit
type git ch
sleep 1500
key Alt+=
waitfor cherry-pick
sleep 1500
waitidle
capture {out}/screenshot-shell.png
'''),
}


def remove_tree(path):
    # git's object files are read-only, which rmtree can't delete until the flag is off
    def writable(func, p, _):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    shutil.rmtree(path, onerror=writable)


def main():
    sys.stdout.reconfigure(encoding="utf-8")  # moss logs carry the λ prompt
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene', default='all', choices=sorted(SCENES) + ['all'])
    ap.add_argument('--size', default='')  # default: the scene's own
    ap.add_argument('--dpi', default='144')
    ap.add_argument('--keep-scratch', action='store_true')
    a = ap.parse_args()
    moss = os.path.join(MOSS, 'build', 'moss.exe')
    fake = os.path.join(HERE, 'fake_claude.py')
    git = shutil.which('git')
    if not os.path.exists(moss):
        sys.exit('no ' + moss + ' (run build.bat)')
    if ' ' in sys.executable or ' ' in fake:
        sys.exit('python and fake_claude.py need paths without spaces (MOSS_CLAUDE is a command line)')

    # --- scratch home and fake sessions ---
    if os.path.exists(SCRATCH):
        remove_tree(SCRATCH)
    home = os.path.join(SCRATCH, 'home')
    cfg = os.path.join(SCRATCH, 'claudecfg')
    os.makedirs(home)
    for sid, cwd, title, age in SESSIONS:
        folder = os.path.join(cfg, 'projects', re.sub('[^A-Za-z0-9]', '-', cwd))
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, sid + '.jsonl')
        with open(path, 'w', encoding='utf-8') as f:
            # compact separators: moss looks for "key":"value" with no spaces
            f.write(json.dumps({'type': 'user', 'cwd': cwd, 'sessionId': sid,
                                'message': {'role': 'user', 'content': title}}, separators=(',', ':')) + '\n')
            f.write(json.dumps({'type': 'ai-title', 'aiTitle': title, 'sessionId': sid}, separators=(',', ':')) + '\n')
            # the agent status bar shows the model of the last reply
            f.write(json.dumps({'type': 'assistant', 'sessionId': sid, 'message': {'role': 'assistant', 'model': 'claude-opus-5-5',
                                'content': [{'type': 'text', 'text': 'On it.'}]}}, separators=(',', ':')) + '\n')
        os.utime(path, (NOW - age, NOW - age))
    # a made-up claude.ai login for the status bar, as Claude Code's /status reads it (no real account)
    with open(os.path.join(cfg, '.claude.json'), 'w', encoding='utf-8') as f:
        json.dump({'oauthAccount': {'emailAddress': 'you@example.com', 'organizationName': ''}}, f)
    with open(os.path.join(cfg, '.credentials.json'), 'w', encoding='utf-8') as f:
        json.dump({'claudeAiOauth': {'subscriptionType': 'max'}}, f)

    # --- the fake codebase and the sessions' folders, at paths that read well ---
    fake_dirs = sorted(set([MESHKIT] + [cwd for _, cwd, _, _ in SESSIONS]))
    if any(os.path.exists(d) for d in fake_dirs):
        sys.exit('one of %s already exists; not touching it' % ', '.join(fake_dirs))
    made_src = not os.path.exists('C:\\src')
    shutil.copytree(os.path.join(HERE, 'meshkit'), MESHKIT)
    for d in fake_dirs:
        os.makedirs(d, exist_ok=True)
    try:
        git_env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull,
                       GIT_AUTHOR_NAME='meshkit', GIT_AUTHOR_EMAIL='meshkit@example.com',
                       GIT_COMMITTER_NAME='meshkit', GIT_COMMITTER_EMAIL='meshkit@example.com')
        subprocess.run([git, 'init', '-q', '-b', 'main'], cwd=MESHKIT, env=git_env, check=True)
        dir_stamps = {}
        for date, message, files in MESHKIT_HISTORY:
            stamp = time.mktime(time.strptime(date, '%Y-%m-%dT%H:%M:%S'))
            for name in files:
                os.utime(os.path.join(MESHKIT, name), (stamp, stamp))
                dir_stamps[os.path.dirname(name)] = stamp
            subprocess.run([git, 'add'] + files, cwd=MESHKIT, env=git_env, check=True)
            subprocess.run([git, 'commit', '-q', '-m', message], cwd=MESHKIT, check=True,
                           env=dict(git_env, GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date))
        with open(os.path.join(MESHKIT, 'src', 'bvh.cpp'), 'a', encoding='utf-8') as f:
            f.write('\n// TODO: refit instead of rebuilding when only the vertices moved\n')  # the prompt's *
        dir_stamps['.git'] = stamp
        for name, s in dir_stamps.items():
            if name:
                os.utime(os.path.join(MESHKIT, name), (s, s))

        # --- the game project for the explorer: files at their sizes, folders dated by their newest file ---
        newest = {}
        for rel, size, date in GAME_FILES:
            path = os.path.join(GAME, *rel.split('/'))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'wb') as f:
                f.truncate(size)
            s = time.mktime(time.strptime(date + 'T12:00:00', '%Y-%m-%dT%H:%M:%S'))
            os.utime(path, (s, s))
            parts = rel.split('/')
            for i in range(1, len(parts)):
                folder = os.path.join(GAME, *parts[:i])
                newest[folder] = max(newest.get(folder, 0), s)
        subprocess.run([git, 'init', '-q', '-b', 'main'], cwd=GAME, env=git_env, check=True)
        subprocess.run([git, 'add', '-A'], cwd=GAME, env=git_env, check=True)
        subprocess.run([git, 'commit', '-q', '-m', 'Motion matching prototype'], cwd=GAME, check=True,
                       env=dict(git_env, GIT_AUTHOR_DATE='2026-06-15T09:30:00', GIT_COMMITTER_DATE='2026-06-15T09:30:00'))
        newest[os.path.join(GAME, '.git')] = time.mktime(time.strptime('2026-06-15T09:30:00', '%Y-%m-%dT%H:%M:%S'))
        for folder in sorted(newest, key=len, reverse=True):
            os.utime(folder, (newest[folder], newest[folder]))

        # --- run each scene headless, from the codebase so the first shell starts there ---
        env = {k: v for k, v in os.environ.items()
               if not k.startswith('CLAUDE_CODE') and k not in ('CLAUDECODE', 'LS_COLORS', 'CLAUDE_CONFIG_DIR')}
        env['USERPROFILE'] = home
        env['CLAUDE_CONFIG_DIR'] = cfg
        env['MOSS_CLAUDE'] = sys.executable + ' ' + fake
        os.makedirs(OUT, exist_ok=True)
        for name in (sorted(SCENES) if a.scene == 'all' else [a.scene]):
            out_file, size, script = SCENES[name]
            script_path = os.path.join(SCRATCH, name + '.txt')
            with open(script_path, 'w', encoding='utf-8', newline='\n') as f:
                f.write(script.replace('{out}', OUT.replace('\\', '/')))
            r = subprocess.run([moss, '--headless', '--size', a.size or size, '--dpi', a.dpi, '--script', script_path],
                               cwd=MESHKIT, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace')
            ok = r.returncode == 0 and os.path.exists(os.path.join(OUT, out_file))
            print(('ok   ' if ok else 'FAIL ') + name + ' -> content\\' + out_file)
            if not ok:
                print('\n'.join(r.stdout.splitlines()[-25:]))
                sys.exit(1)
    finally:
        # --- clean up ---
        for d in (['C:\\src'] if made_src else fake_dirs):
            try:
                remove_tree(d)
            except OSError as e:
                print('could not remove', d, '-', e)
        if not a.keep_scratch:
            remove_tree(SCRATCH)


main()
