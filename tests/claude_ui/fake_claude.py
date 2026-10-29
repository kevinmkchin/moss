# A stand-in for claude: registered like Claude Code (CLAUDE_CONFIG_DIR\sessions\<pid>.json) with the session it
# resumes, its screen drawn as claude draws it (what it said, then its prompt box: a rule, the "❯" line, a rule, the
# mode line). Each line it hears is a turn: busy (a spinner in its title, a status line over the box), the prompt, a
# command run and a reply written to the session's transcript. "ask" shows a question in place of the box until the next
# line; a line starting with / is only heard.
import glob, json, os, sys, threading, time
sid = sys.argv[sys.argv.index('--resume') + 1] if '--resume' in sys.argv else ''
cfg = os.environ.get('CLAUDE_CONFIG_DIR', '')
if sid and cfg:
    folder = os.path.join(cfg, 'sessions')
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, '%d.json' % os.getpid()), 'w') as f:
        json.dump({'pid': os.getpid(), 'sessionId': sid, 'cwd': os.getcwd()}, f, separators=(',', ':'))
lock = threading.RLock()
said = ['fake-claude ready']
state = {'mode': False, 'status': '', 'asking': False, 'turn': 0, 'notice': False}

def draw():
    with lock:
        try:
            cols, rows = os.get_terminal_size()
        except OSError:
            cols, rows = 100, 30
        out = ['\x1b[?2026h\x1b[2J\x1b[H']
        top = said[-(rows - 8):]
        for k, line in enumerate(top):
            out.append('\x1b[%d;1H%s' % (k + 1, line[:cols - 1]))
        y = len(top) + 2
        if state['status']:
            out.append('\x1b[%d;1H%s' % (y, state['status']))
            y += 1
        rule = '─' * (cols - 2)
        if state['asking']:
            for line in [rule, ' Bash command', '   rm -rf build', ' Do you want to proceed?', ' ❯ 1. Yes', '   2. No', ' Esc to cancel']:
                out.append('\x1b[%d;1H%s' % (y, line))
                y += 1
        else:
            out.append('\x1b[%d;1H%s\x1b[%d;1H❯ \x1b[%d;1H%s' % (y, rule, y + 1, y + 2, rule))
            if state['mode']:
                out.append('\x1b[%d;1H  ⏵⏵ auto mode on (shift+tab to cycle)%s' % (y + 3, ' · esc to interrupt' if state['status'] else ''))
            if state['notice']:
                # a usage warning under the mode line, as claude shows one
                out.append('\x1b[%d;1H  You\'ve used 85%% of your weekly limit · resets Mon 9am\x1b[%d;1H  /upgrade to keep using Claude Code' % (y + 4, y + 5))
            out.append('\x1b[%d;3H' % (y + 1))
        out.append('\x1b[?2026l')
        sys.stdout.write(''.join(out))
        sys.stdout.flush()

def title(busy):
    sys.stdout.write('\x1b]0;%s fake claude\x07' % ('◐' if busy else '✳'))
    sys.stdout.flush()

def transcript():
    hits = glob.glob(os.path.join(cfg, 'projects', '*', sid + '.jsonl')) if sid and cfg else []
    return hits[0] if hits else ''

def write(entry):
    path = transcript()
    if not path:
        return
    # dated against the headless clock (2026-06-15 12:00 UTC), a minute apart
    state['written'] = state.get('written', 0) + 1
    entry.update({'cwd': os.getcwd(), 'sessionId': sid, 'timestamp': '2026-06-15T11:%02d:00.000Z' % state['written']})
    with open(path, 'a', encoding='utf-8') as f:
        f.write(json.dumps(entry, separators=(',', ':')) + '\n')

def turn(text, n):
    # the prompt, a command claude runs (its result a moment later), then its reply
    with lock:
        title(True)
        state['status'] = '· Pondering… (esc to interrupt)'
        draw()
    write({'type': 'user', 'message': {'role': 'user', 'content': text}})
    time.sleep(0.8)
    write({'type': 'assistant', 'message': {'role': 'assistant', 'model': 'claude-opus-5-5', 'content': [
        {'type': 'tool_use', 'id': 'toolu_t%d' % n, 'name': 'Bash', 'input': {'command': 'echo turn %d' % n}}]}})
    with lock:
        said.append('● Bash(echo turn %d)' % n)
        draw()
    time.sleep(0.8)
    write({'type': 'user', 'message': {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': 'toolu_t%d' % n, 'content': 'turn %d' % n}]}})
    time.sleep(0.8)
    reply = 'You said **%s**. Here it is as code:\n\n```text\n%s\n```' % (text.replace('\n', ' / '), text)
    write({'type': 'assistant', 'message': {'role': 'assistant', 'model': 'claude-opus-5-5', 'content': [{'type': 'text', 'text': reply}]}})
    with lock:
        said.append('● You said %s.' % text.replace('\n', ' / '))
        state['status'] = ''
        title(False)
        draw()

def mode_line():
    # the mode line, drawn a moment later as claude does after the tab's size settles (its account line)
    with lock:
        state['mode'] = True
        draw()

title(False)
draw()
threading.Timer(2.0, mode_line).start()
pasting = []
for raw in sys.stdin:
    line = raw.rstrip('\r\n')
    # a paste's lines come as lines of their own, between its brackets
    if '\x1b[200~' in line and '\x1b[201~' not in line:
        pasting = [line.replace('\x1b[200~', '')]
        continue
    if pasting:
        if '\x1b[201~' not in line:
            pasting.append(line)
            continue
        pasting.append(line.replace('\x1b[201~', ''))
        line = '\n'.join(pasting)
        pasting = []
    line = line.replace('\x1b[200~', '').replace('\x1b[201~', '').strip()
    with lock:
        said.append('fake-claude heard: ' + line.replace('\n', ' / '))
        if state['asking']:
            state['asking'] = False
            draw()
            continue
        if line == 'ask':
            state['asking'] = True
            draw()
            continue
        if line == 'notice':
            state['notice'] = True
            draw()
            continue
        draw()
    if line and not line.startswith('/'):
        state['turn'] += 1
        threading.Thread(target=turn, args=(line, state['turn'])).start()
