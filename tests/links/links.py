# Lines with things to Ctrl+click: a path with a line and column, a script (shown, not run), an OSC 8 link to a
# file, a path that isn't there.
import os, sys
os.makedirs('notes', exist_ok=True)
os.makedirs('tools', exist_ok=True)
open(os.path.join('notes', 'todo.md'), 'w').write('# todo\n\none\ntwo\n')
open(os.path.join('tools', 'go.bat'), 'w').write('@echo go\n')
uri = 'file:///' + os.path.abspath(os.path.join('notes', 'todo.md')).replace('\\', '/').replace(' ', '%20')
sys.stdout.write('see notes\\todo.md:3:2 for more\n')
sys.stdout.write('run tools\\go.bat now\n')
sys.stdout.write('read \x1b]8;;' + uri + '\x1b\\the notes\x1b]8;;\x1b\\ here\n')
sys.stdout.write('gone missing\\nothere.txt too\n')
sys.stdout.flush()
