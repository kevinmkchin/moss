#!/bin/sh
# Runs one test set headless: tests/<set>/<set>.txt with out/ fresh, then compares out/ with reference/.
#   tests/run_set.sh <set>          run and compare
#   tests/run_set.sh <set> bless    make the last run's out/ the reference
# A set may have its own run.sh (several runs, e.g. session); it uses this for the compare.
Set="$1"
Dir="$(cd "$(dirname "$0")" && pwd)/$Set"
Root="$(cd "$(dirname "$0")/.." && pwd)"
Exe="$Root/build/moss.exe"
cd "$Dir" || exit 1
if [ "$2" = "bless" ]; then
    # captures named *_varies.png change every run (timings, counters): looked at, never compared
    rm -rf reference && mkdir reference && cp out/dumps.txt reference/ 2>/dev/null
    for F in out/*.png; do case "$F" in *_varies.png) ;; *) cp "$F" reference/ ;; esac; done
    echo "$Set: blessed $(ls reference | wc -l) files"
    exit 0
fi
if [ "$2" != "compare" ]; then
    mkdir -p out && rm -rf out/*
    # the shells run in a folder outside any git repo, so their prompts are the same every run
    Work="${TEMP:-/tmp}/term-tests/$Set"
    rm -rf "$Work" && mkdir -p "$Work/out"
    # no Claude Code sessions but the ones a set makes: the sidebar lists the same every run
    if [ -z "$CLAUDE_CONFIG_DIR" ]; then mkdir -p "$Work/noclaude"; export CLAUDE_CONFIG_DIR="$(cygpath -w "$Work/noclaude")"; fi
    cp "$Dir"/*.txt "$Work"/ 2>/dev/null
    cp "$Dir"/*.py "$Work"/ 2>/dev/null
    (cd "$Work" && timeout 180 "$Exe" --headless --size 1280x800 --script "$Set.txt" > out/log.txt 2>&1)
    Code=$?
    cp "$Work"/out/* out/
    # the dumps that should be the same every run: layouts and tabs without their volatile fields, and what a run
    # would have opened
    sed -n -E 's/^\[[0-9]+\] dump: [^:]*: //p; /^(pane|  |side|stacked|\(\*)/p; s/^\[[0-9]+\] headless: [^:]*: ((open|show|nothing to open) .*)$/\1/p' out/log.txt |
        sed -E 's/ \| cpu [0-9]+%//; s/ \| key [-0-9a-f]+//' > out/dumps.txt
    if [ $Code -ne 0 ]; then
        echo "$Set: FAILED (exit $Code)"
        grep -E "timed out|not found|DIFFERENT|unknown|CRASH|ASSERT" out/log.txt | head -5
    fi
fi
python "$Root/tools/compare_out.py" "$Dir" 8 | sed "s/^/$Set: /"
