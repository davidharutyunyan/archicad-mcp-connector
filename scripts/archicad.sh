#!/usr/bin/env bash
# Controls the local Archicad instance used for development and live tests.
#   archicad.sh start [project.pln|template.tpl]   launch (default: fresh project from the default template)
#   archicad.sh stop                               save the open project (if it has a file), then quit (falls back to kill);
#                                                  refuses to quit when that save fails, unless ARCHICAD_DISCARD=1
#   archicad.sh restart [project]                  stop + start + wait
#   archicad.sh wait                               wait until the JSON API answers
#   archicad.sh status                             print API status
source "$(dirname "$0")/lib.sh"

cmd="${1:-status}"
project="${2:-}"

wait_up() {
  local timeout="${1:-180}" waited=0
  until archicad_is_up; do
    sleep 2; waited=$((waited + 2))
    if (( waited > 20 )) && [[ -z "$(archicad_pids)" ]]; then
      echo "Archicad exited during startup (a startup dialog may have been closed with 'Exit')." >&2; return 1
    fi
    if (( waited % 30 == 0 )); then
      echo "  still waiting for the JSON API (${waited}s) - if Archicad shows a dialog, answer it (e.g. 'Proceed')" >&2
    fi
    if (( waited >= timeout )); then echo "Archicad JSON API did not come up within ${timeout}s (a modal dialog is probably open)" >&2; return 1; fi
  done
  # Give the add-ons a moment to finish registering commands.
  sleep 2
  echo "Archicad is up on port $ARCHICAD_PORT"
}

# Path of the open project's file, empty for untitled projects (or when the API does not answer).
open_project_path() {
  ac_addon GetProjectInfo '{}' 30 2>/dev/null | python3 -c '
import json, sys
try:
    r = json.load(sys.stdin)["result"]["addOnCommandResponse"]
    print("" if r.get("untitled", True) else (r.get("file") or {}).get("path", ""))
except Exception:
    print("")'
}

# QuitArchicad discards unsaved changes, so a project that has a file is saved first. After a restart Archicad
# sometimes opens the file read-only (stale lock): Save As onto the same path clears that (verified live).
# Returns 1 when the project could not be saved.
save_open_project() {
  local path r
  path=$(open_project_path)
  [[ -n "$path" ]] || return 0
  r=$(ac_addon SaveProject '{}' 120 2>/dev/null || true)
  [[ "$r" == *'"saved":true'* ]] && { echo "Saved $path"; return 0; }
  r=$(ac_addon SaveProjectAs "$(python3 -c 'import json,sys; print(json.dumps({"path": sys.argv[1], "overwrite": True}))' "$path")" 120 2>/dev/null || true)
  [[ "$r" == *'"saved":true'* ]] && { echo "Saved $path (Save As onto the same path)"; return 0; }
  echo "Could not save $path: $r" >&2
  return 1
}

stop_ac() {
  if archicad_is_up && [[ "${ARCHICAD_DISCARD:-0}" != "1" ]]; then
    save_open_project || { echo "Not quitting Archicad: the open project has unsaved work that could not be saved (ARCHICAD_DISCARD=1 to quit anyway)." >&2; return 1; }
  fi
  if [[ -z "$(archicad_pids)" ]]; then
    # A crash leaves the GSReport bug reporter (and a lock file) behind; clear them before the next start.
    pkill -9 -f "GSReport" 2>/dev/null || true
    rm -f "$HOME/Library/Application Support/Graphisoft/"*"-$ARCHICAD_VERSION/@Lock.T" 2>/dev/null || true
    echo "Archicad is not running"; return 0
  fi
  if archicad_is_up; then
    local r
    r=$(ac_addon QuitArchicad '{"confirm":true}' 10 2>/dev/null || true)
    if [[ "$r" == *'not found'* || "$r" == *'"succeeded":false'* ]]; then
      # Our add-on is not loaded in this instance: try Tapir's quit command.
      ac_post '{"command":"API.ExecuteAddOnCommand","parameters":{"addOnCommandId":{"commandNamespace":"TapirCommand","commandName":"QuitArchicad"},"addOnCommandParameters":{}}}' 10 >/dev/null 2>&1 || true
    fi
  fi
  local waited=0
  while [[ -n "$(archicad_pids)" ]] && (( waited < 30 )); do sleep 1; waited=$((waited + 1)); done
  if [[ -n "$(archicad_pids)" ]]; then
    echo "Graceful quit timed out; killing Archicad"
    kill -9 $(archicad_pids) 2>/dev/null || true
    sleep 1
  fi
  pkill -9 -f "GSReport" 2>/dev/null || true
  rm -f "$HOME/Library/Application Support/Graphisoft/"*"-$ARCHICAD_VERSION/@Lock.T" 2>/dev/null || true
  echo "Archicad stopped"
}

start_ac() {
  local target="${project:-$(archicad_template)}"
  [[ -n "$target" ]] || { echo "No project/template to open"; exit 1; }
  echo "Starting Archicad with: $target"
  open -a "$(archicad_app)" "$target"
  wait_up 240
  # A reopened .pln can come up read-only (stale lock of the previous session): take it over right away,
  # so later saves (and the next stop) work.
  if [[ "$target" == *.pln ]]; then
    local r
    r=$(ac_addon SaveProjectAs "$(python3 -c 'import json,sys; print(json.dumps({"path": sys.argv[1], "overwrite": True}))' "$target")" 120 2>/dev/null || true)
    [[ "$r" == *'"saved":true'* ]] || echo "Warning: could not re-save $target after opening (it may be read-only): $r" >&2
  fi
}

case "$cmd" in
  start) start_ac ;;
  stop) stop_ac ;;
  restart) stop_ac; start_ac ;;
  wait) wait_up "${2:-180}" ;;
  status) if archicad_is_up; then ac_post '{"command":"API.GetProductInfo"}'; echo; ac_addon GetAddOnInfo; echo; else echo "Archicad JSON API is not reachable on port $ARCHICAD_PORT"; fi ;;
  *) echo "usage: archicad.sh start|stop|restart|wait|status [project]"; exit 1 ;;
esac
