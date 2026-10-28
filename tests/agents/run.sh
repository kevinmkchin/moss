#!/bin/sh
# Agents and palette: claude faked by a command that prints "fake-claude" and stays a shell.
cd "$(dirname "$0")" || exit 1
MOSS_CLAUDE="python fake_claude.py" sh ../run_set.sh agents "$1"
