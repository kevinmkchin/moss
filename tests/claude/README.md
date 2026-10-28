Real Claude Code in an agent tab (uses the account's tokens, so it isn't in run_all.sh). Run it in a
folder outside any repo, then delete its project folder from ~/.claude/projects (the test made it):

    mkdir -p /tmp/claudetest/out && cd /tmp/claudetest && cp <term>/tests/claude/claude.txt . &&
    <term>/build/term.exe --headless --script claude.txt | cat

It always ends with /exit: a claude killed while starting adds strikes that turn its fullscreen
renderer off machine-wide.
