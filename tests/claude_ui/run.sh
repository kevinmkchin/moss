#!/bin/sh
# Claude tabs made comfortable: the composer, the reader, the account line, the sidebar's search and menu. Sessions are
# made up in a config folder of the run's (CLAUDE_CONFIG_DIR), claude is faked.
cd "$(dirname "$0")" || exit 1
Work="${TEMP:-/tmp}/term-tests/claude_ui"
W="$(cygpath -w "$Work" 2>/dev/null || echo "$Work")"
CLAUDE_CONFIG_DIR="$W\\claudecfg" MOSS_CLAUDE="python $W\\fake_claude.py" sh ../run_set.sh claude_ui "$1"
# what was copied (the code block), for the compare
[ "$1" = "bless" ] || grep -a "headless: clipboard:" out/log.txt -A6 | sed 's/^\[[0-9]*\] //' >> out/dumps.txt
