#!/bin/sh
# Every headless test set (build first). Each prints its result lines; out/ and reference/ stay local.
cd "$(dirname "$0")" || exit 1
for Set in smoke layout fold terminal browser goto view focus cloud background settings find tabs links shells; do sh run_set.sh $Set "$1"; done
sh agents/run.sh "$1"
sh sessions/run.sh "$1"
sh session/run.sh "$1"
sh windows/run.sh "$1"
sh accounts/run.sh "$1"
