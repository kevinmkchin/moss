#!/bin/sh
# Windows: a tab torn off the main window belongs to the window w1 (the server keeps whose a tab is), w1 shows
# just that tab, and closing w1 hands it back. Three runs against one named server, then the usual compare.
cd "$(dirname "$0")" || exit 1
if [ "$1" = "bless" ]; then sh ../run_set.sh windows bless; exit 0; fi
mkdir -p out && rm -rf out/*
Exe="$(cd ../.. && pwd)/build/moss.exe"
Here="$(pwd)"
Work="${TEMP:-/tmp}/term-tests/windows"
rm -rf "$Work" && mkdir -p "$Work/out" && cp *.txt "$Work"/ && cd "$Work" || exit 1
# no Claude Code sessions: the sidebar lists the same every run
mkdir -p "$Work/noclaude" && export CLAUDE_CONFIG_DIR="$(cygpath -w "$Work/noclaude")"
Instance=windows-test-$$
timeout 60 $Exe --headless --instance $Instance --session out/main.json --script main1.txt > out/main1.txt 2>&1 || echo "windows: main1 FAILED"
timeout 60 $Exe --headless --instance $Instance --window w1 --session out/w1.json --script w1.txt > out/w1.txt 2>&1 || echo "windows: w1 FAILED"
timeout 60 $Exe --headless --instance $Instance --session out/main.json --script main2.txt > out/log.txt 2>&1 || echo "windows: main2 FAILED"
cat out/main1.txt out/w1.txt out/log.txt | grep -E "timed out|not found|CRASH|ASSERT" | head -5
cat out/main1.txt out/w1.txt out/log.txt | sed -n -E 's/^\[[0-9]+\] dump: [^:]*: //p; /^(pane|  |side|stacked|\(\*)/p; s/^\[[0-9]+\] headless: [^:]*: (open window .*|tabs handed back.*)$/\1/p' | sed -E 's/ \| cpu [0-9]+%//; s/ \| key [-0-9a-f]+//' > out/dumps.txt
cp out/* "$Here/out/"
cd "$Here" && sh ../run_set.sh windows compare
