# Made-up Claude Code sessions for the claude_ui test, in CLAUDE_CONFIG_DIR (run.sh points it here): one with markdown
# (a heading, bold, `code`, a list, a code block, a table) and its reply's token usage, one whose later message has a
# word only a full-text search finds, and one plain. Dated against the headless clock (2026-06-15 12:00 UTC).
import calendar, json, os, re

cfg = os.environ['CLAUDE_CONFIG_DIR']
now = calendar.timegm((2026, 6, 15, 12, 0, 0))
reply = '\n'.join([
    '# The plan',
    'We **change two files** and keep `main` green:',
    '- the parser',
    '- the `tests/` folder',
    '',
    '```c',
    'int add(int a, int b)',
    '{',
    '    return a + b;',
    '}',
    '```',
    '',
    '| file | lines |',
    '|------|------:|',
    '| parse.c | 120 |',
    '| tests.c | 48 |',
])
sessions = [
    ('aaaaaaaa-0000-4000-8000-000000000001', 'markdown', 'Show me the plan', reply, 3600),
    ('aaaaaaaa-0000-4000-8000-000000000002', 'zoo', 'Tidy the enclosure list', 'Done. The zebracorn pen is the last one now.', 2 * 86400),
    ('aaaaaaaa-0000-4000-8000-000000000003', 'plain', 'Rename the build step', 'Renamed it to build-all.', 3 * 86400),
]
for sid, project, prompt, text, age in sessions:
    cwd = os.path.join(os.getcwd(), 'projects', project)
    os.makedirs(cwd, exist_ok=True)
    folder = os.path.join(cfg, 'projects', re.sub('[^A-Za-z0-9]', '-', cwd))
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, sid + '.jsonl')
    with open(path, 'w') as f:
        f.write(json.dumps({'type': 'user', 'cwd': cwd, 'sessionId': sid, 'timestamp': '2026-06-15T10:00:00.000Z',
                            'message': {'role': 'user', 'content': prompt}}, separators=(',', ':')) + '\n')
        if project == 'markdown':
            # tools it used first, in a row: a command, an edit and a search that failed, and their results (a result's
            # keys in Claude Code's order: its content before its tool's id)
            f.write(json.dumps({'type': 'assistant', 'cwd': cwd, 'sessionId': sid, 'timestamp': '2026-06-15T10:00:02.000Z',
                                'message': {'role': 'assistant', 'model': 'claude-opus-5-5', 'content': [
                                    {'type': 'tool_use', 'id': 'toolu_1', 'name': 'Bash', 'input': {'command': 'git status', 'description': 'Status'}},
                                    {'type': 'tool_use', 'id': 'toolu_2', 'name': 'Edit', 'input': {'file_path': 'C:\\src\\parse.c', 'old_string': 'int add(int a)', 'new_string': 'int add(int a, int b)'}},
                                    {'type': 'tool_use', 'id': 'toolu_3', 'name': 'Grep', 'input': {'pattern': 'add\\(', 'path': 'C:\\src\\nowhere'}}]}},
                               separators=(',', ':')) + '\n')
            f.write(json.dumps({'type': 'user', 'cwd': cwd, 'sessionId': sid, 'timestamp': '2026-06-15T10:00:03.000Z',
                                'message': {'role': 'user', 'content': [
                                    {'type': 'tool_result', 'tool_use_id': 'toolu_1', 'content': 'On branch main\nnothing to commit, working tree clean'},
                                    {'type': 'tool_result', 'tool_use_id': 'toolu_2', 'content': [{'type': 'text', 'text': 'The file was updated.'}]},
                                    {'type': 'tool_result', 'content': 'Path does not exist: C:\\src\\nowhere', 'is_error': True, 'tool_use_id': 'toolu_3'}]}},
                               separators=(',', ':')) + '\n')
        f.write(json.dumps({'type': 'assistant', 'cwd': cwd, 'sessionId': sid, 'timestamp': '2026-06-15T10:00:05.000Z',
                            'message': {'role': 'assistant', 'model': 'claude-opus-5-5', 'content': [{'type': 'text', 'text': text}],
                                        'usage': {'input_tokens': 1200, 'cache_creation_input_tokens': 800, 'cache_read_input_tokens': 40000, 'output_tokens': 300}}},
                           separators=(',', ':')) + '\n')
        if project == 'markdown':
            # then a /command with its output, and a prompt the user interrupted
            for content in ['<command-name>/cost</command-name>\n            <command-message>cost</command-message>\n            <command-args></command-args>',
                            '<local-command-stdout>Total cost: $0.42\nTotal duration: 3m 12s</local-command-stdout>']:
                f.write(json.dumps({'parentUuid': 'x', 'isSidechain': False, 'type': 'system', 'subtype': 'local_command', 'content': content,
                                    'level': 'info', 'timestamp': '2026-06-15T10:00:06.000Z', 'isMeta': False, 'cwd': cwd, 'sessionId': sid},
                                   separators=(',', ':')) + '\n')
            f.write(json.dumps({'type': 'user', 'cwd': cwd, 'sessionId': sid, 'timestamp': '2026-06-15T10:00:07.000Z',
                                'message': {'role': 'user', 'content': [{'type': 'text', 'text': '[Request interrupted by user]'}]}},
                               separators=(',', ':')) + '\n')
    os.utime(path, (now - age, now - age))
with open(os.path.join(cfg, '.claude.json'), 'w') as f:
    json.dump({'oauthAccount': {'emailAddress': 'tester@example.com', 'organizationName': 'Example Org'}}, f, indent=2)
print('made')
