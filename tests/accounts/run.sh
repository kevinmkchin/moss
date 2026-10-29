#!/bin/sh
# Accounts: Claude accounts and their folder rules (accounts.txt): the right account starts, the wrong one is held,
# and every claude comes back under its own after the server
# restarts (a named server, quit between two runs: tabs.txt). Claude is faked. Two runs, then the usual compare.
cd "$(dirname "$0")" || exit 1
if [ "$1" = "bless" ]; then sh ../run_set.sh accounts bless; exit 0; fi
mkdir -p out && rm -rf out/*
Exe="$(cd ../.. && pwd)/build/moss.exe"
Here="$(pwd)"
# the shells run outside any git repo, so their prompts are the same every run
Work="${TEMP:-/tmp}/term-tests/accounts"
rm -rf "$Work" && mkdir -p "$Work/out" && cp *.txt *.py "$Work"/ && cd "$Work" || exit 1
WinWork="$(cygpath -w "$Work")"
export CLAUDE_CONFIG_DIR="$WinWork\\cfg1"
export MOSS_CLAUDE="python $WinWork\\fake_claude.py"
export MOSS_ACCOUNTS_FILE="$WinWork\\accounts.txt"
Instance=accounts-test-$$
timeout 60 $Exe --headless --instance $Instance --session out/session.json --script first.txt > out/first.txt 2>&1
Code1=$?
timeout 60 $Exe --headless --instance $Instance --session out/session.json --script second.txt > out/log.txt 2>&1
Code=$?
rm -rf "$LOCALAPPDATA/moss/tabs-$Instance.txt" "$LOCALAPPDATA/moss/history-$Instance"   # the named server's files
[ $Code1 -ne 0 ] && echo "accounts: first run FAILED (exit $Code1)" && grep -E "timed out|not found|CRASH|ASSERT" out/first.txt | head -5
[ $Code -ne 0 ] && echo "accounts: FAILED (exit $Code)" && grep -E "timed out|not found|CRASH|ASSERT" out/log.txt | head -5
# what each run found: the claude tab's agent and config folder, without volatile fields
sed -n -E 's/^\[[0-9]+\] dump: [^:]*: (tab [0-9]+: .*agent claude.*)$/\1/p' out/first.txt out/log.txt |
    sed -E 's/ \| cpu [0-9]+%//; s/ \| key [-0-9a-f]+//; s/ \| cwd [^|]*//' > out/dumps.txt
# the added account's folder: the settings copied, never the login (.claude.json, .credentials.json)
(cd accounts/side 2>/dev/null && ls -A | sed 's/^/side: /') >> out/dumps.txt
grep -E "^account|^theme|^folder" accounts.txt | sed -E 's/= .*\\(cfg2|accounts\\side)$/= \1/; s/^folder .*\\dev /folder dev /' >> out/dumps.txt
cp out/* "$Here/out/"
cd "$Here" && sh ../run_set.sh accounts compare
