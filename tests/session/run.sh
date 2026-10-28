#!/bin/sh
# Session: a layout saved by one window comes back in the next, with its tabs (a named server that outlives
# the first run). Two runs, then the usual compare.
cd "$(dirname "$0")" || exit 1
if [ "$1" = "bless" ]; then sh ../run_set.sh session bless; exit 0; fi
mkdir -p out && rm -rf out/*
Exe="$(cd ../.. && pwd)/build/moss.exe"
Here="$(pwd)"
# the shells run outside any git repo, so their prompts are the same every run
Work="${TEMP:-/tmp}/term-tests/session"
rm -rf "$Work" && mkdir -p "$Work/out" && cp *.txt "$Work"/ && cd "$Work" || exit 1
# no Claude Code sessions: the sidebar lists the same every run
mkdir -p "$Work/noclaude" && export CLAUDE_CONFIG_DIR="$(cygpath -w "$Work/noclaude")"
Instance=session-test-$$
timeout 60 $Exe --headless --instance $Instance --session out/session.json --script first.txt > out/first.txt 2>&1
timeout 60 $Exe --headless --instance $Instance --session out/session.json --script second.txt > out/log.txt 2>&1
Code=$?
sed -n -E '/^(pane|  |side|stacked|\(\*)/p' out/first.txt > out/first_layout.txt
sed -n -E '/^(pane|  |side|stacked|\(\*)/p' out/log.txt > out/second_layout.txt
if diff out/first_layout.txt out/second_layout.txt > /dev/null; then echo "session: layout came back"; else echo "session: DIFFERENT layout after restart"; diff out/first_layout.txt out/second_layout.txt; fi
[ $Code -ne 0 ] && echo "session: FAILED (exit $Code)"
cp out/second_layout.txt out/dumps.txt
cp out/* "$Here/out/"
cd "$Here" && sh ../run_set.sh session compare
