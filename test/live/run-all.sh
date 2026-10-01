#!/usr/bin/env bash
# Full live regression: rebuild + reinstall the add-on, restart Archicad on a fresh project from
# the template, then run every live suite in order (the disruptive project suite last).
#   bash test/live/run-all.sh            (use --no-build to skip the add-on rebuild)
set -uo pipefail
cd "$(dirname "$0")/../.."
# Node 20+ must be on PATH; NODE_BIN=<dir of node> prepends another one (e.g. an nvm install).
[[ -n "${NODE_BIN:-}" ]] && export PATH="$NODE_BIN:$PATH"
if [[ "${1:-}" != "--no-build" ]]; then bash scripts/dev-cycle.sh >/dev/null 2>&1 || { echo "dev-cycle failed"; exit 1; }
else bash scripts/archicad.sh restart >/dev/null 2>&1 || { echo "restart failed"; exit 1; }; fi
total_fail=0
mkdir -p test/.tmp
LOG=test/.tmp/live-run.log
: > "$LOG"
for f in test/live/[0-9][0-9]-*.live.ts; do
  out=$(npx tsx "$f" 2>&1)
  echo "$out" >> "$LOG"
  echo "$out" | grep -E "FAIL|^==" 
  echo "$out" | grep -q "failed" && [[ "$(echo "$out" | grep -E '^==' | sed -E 's/.* ([0-9]+) failed/\1/')" != "0" ]] && total_fail=$((total_fail+1))
done
echo "suites with failures: $total_fail (full log: $LOG)"
