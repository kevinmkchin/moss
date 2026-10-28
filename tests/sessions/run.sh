#!/bin/sh
# Sessions sidebar: Claude Code's sessions come from a made-up config folder (CLAUDE_CONFIG_DIR, filled by
# make_sessions.py in the run's folder), claude is faked.
cd "$(dirname "$0")" || exit 1
Work="${TEMP:-/tmp}/term-tests/sessions"
CLAUDE_CONFIG_DIR="$(cygpath -w "$Work" 2>/dev/null || echo "$Work")\\claudecfg" MOSS_CLAUDE="python $(cygpath -w "$Work" 2>/dev/null || echo "$Work")\fake_claude.py" sh ../run_set.sh sessions "$1"
