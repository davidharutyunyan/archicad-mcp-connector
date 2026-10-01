#!/usr/bin/env bash
# Keeps Archicad running during unattended test sessions: if the process has been gone for
# DOWN_GRACE seconds and no build/install/restart script is active, start a fresh project.
source "$(dirname "$0")/lib.sh"
DOWN_GRACE="${DOWN_GRACE:-60}"
down_since=0
while true; do
  if [[ -n "$(archicad_pids)" ]] || pgrep -f "dev-cycle.sh|install-addon.sh|build-addon.sh|archicad.sh (start|stop|restart)" >/dev/null; then
    down_since=0
  else
    now=$(date +%s)
    (( down_since == 0 )) && down_since=$now
    if (( now - down_since >= DOWN_GRACE )); then
      echo "$(date '+%H:%M:%S') Archicad down for $((now - down_since))s -> restarting"
      pkill -9 -f GSReport 2>/dev/null || true
      bash "$REPO_ROOT/scripts/archicad.sh" start 2>&1 | tail -1
      down_since=0
    fi
  fi
  sleep 10
done
