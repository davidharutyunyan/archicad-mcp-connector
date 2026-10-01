#!/usr/bin/env bash
# Shared helpers for the archicad-connector dev scripts (macOS).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ADDON_DIR="$REPO_ROOT/addon"
BUILD_DIR="${ADDON_BUILD_DIR:-$ADDON_DIR/build}"
BUNDLE_NAME="ClaudeConnector.bundle"
ARCHICAD_VERSION="${ARCHICAD_VERSION:-26}"
ARCHICAD_PORT="${ARCHICAD_PORT:-19723}"

# Archicad installation root, e.g. "/Applications/Graphisoft/Archicad 26"
archicad_root() {
  if [[ -n "${ARCHICAD_ROOT:-}" ]]; then echo "$ARCHICAD_ROOT"; return; fi
  local root="/Applications/Graphisoft/Archicad $ARCHICAD_VERSION"
  [[ -d "$root" ]] || { echo "Archicad $ARCHICAD_VERSION not found at $root (set ARCHICAD_ROOT)" >&2; return 1; }
  echo "$root"
}

archicad_app() {
  if [[ -n "${ARCHICAD_APP:-}" ]]; then echo "$ARCHICAD_APP"; return; fi
  echo "$(archicad_root)/Archicad $ARCHICAD_VERSION.app"
}

# The localized "Archicad Add-Ons" folder: the top-level folder holding the most *.bundle add-ons.
archicad_addons_dir() {
  if [[ -n "${ARCHICAD_ADDONS_DIR:-}" ]]; then echo "$ARCHICAD_ADDONS_DIR"; return; fi
  local root best="" bestCount=0
  root="$(archicad_root)"
  while IFS= read -r -d '' dir; do
    [[ "$dir" == *.app ]] && continue
    local count
    count=$(find "$dir" -maxdepth 3 -name "*.bundle" -type d 2>/dev/null | wc -l | tr -d ' ')
    if (( count > bestCount )); then best="$dir"; bestCount=$count; fi
  done < <(find "$root" -mindepth 1 -maxdepth 1 -type d -print0)
  [[ -n "$best" ]] || { echo "Could not detect the Archicad Add-Ons folder (set ARCHICAD_ADDONS_DIR)" >&2; return 1; }
  echo "$best"
}

# Default template (.tpl) used to start a fresh untitled project.
archicad_template() {
  if [[ -n "${ARCHICAD_TEMPLATE:-}" ]]; then echo "$ARCHICAD_TEMPLATE"; return; fi
  find "$(archicad_root)" -maxdepth 4 -name "*.tpl" -not -path "*.app/*" 2>/dev/null | head -1
}

# Main Archicad process(es). Archicad may run with arguments (e.g. "-RECOVER ..." after a crash),
# so match the executable path followed by a space or the end of the command line.
archicad_pids() {
  pgrep -f "Archicad $ARCHICAD_VERSION.app/Contents/MacOS/Archicad( |$)" || true
}

# POST a raw JSON command to the Archicad JSON API.
ac_post() {
  curl -s -m "${2:-30}" -X POST "http://127.0.0.1:$ARCHICAD_PORT" -H 'Content-Type: application/json' -d "$1"
}

# Call a Claude Connector add-on command: ac_addon <CommandName> [json-params]
ac_addon() {
  local params="${2:-}"
  [[ -n "$params" ]] || params='{}'
  ac_post "{\"command\":\"API.ExecuteAddOnCommand\",\"parameters\":{\"addOnCommandId\":{\"commandNamespace\":\"ClaudeConnector\",\"commandName\":\"$1\"},\"addOnCommandParameters\":$params}}" "${3:-60}"
}

archicad_is_up() {
  local r
  r=$(ac_post '{"command":"API.IsAlive"}' 3 2>/dev/null || true)
  [[ "$r" == *'"isAlive":true'* ]]
}
